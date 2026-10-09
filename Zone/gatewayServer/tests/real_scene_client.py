#!/usr/bin/env python3
# ============================================================================
# 真实链路客户端（网关公网端口）：只依赖标准库，不链接项目代码
#
# 做的事：
#   1. 连网关公网端口，发 ClientFrame 鉴权（playerId + ticket）-> 期待 AuthAck
#   2. 发一条真实业务请求 PLAYER(3)/MOVE(2)，body 是手写的 gs.WalkReq
#      （proto3 小字段号 + 小整数 = 全 varint，手写字节即可，不必引入 protobuf 运行时）
#   3. 校验正常 WalkAck 和无效路由 gs.RetTip 都沿原请求头及 requestId 回传
#
# 用法: real_scene_client.py <publicPort> <playerId> <ticket>
# ============================================================================
import socket
import struct
import sys

MAGIC = 0xC1EB
VERSION = 2
HEADER_LEN = 32

KIND_REQUEST = 1
KIND_RESPONSE = 2
KIND_PUSH = 3
KIND_ERROR = 4
KIND_CONTROL = 5

SYS_MODULE = 1
SYS_AUTH = 1
SYS_AUTH_ACK = 2

MOD_PLAYER = 3
METHOD_MOVE = 2


def client_frame(kind, module, method, request_id, body=b""):
    total = HEADER_LEN + len(body)
    return struct.pack(">HBBHHQI", MAGIC, VERSION, kind, module, method, request_id, total) + bytes(12) + body


def read_frame(sock):
    buf = read_exact(sock, HEADER_LEN)
    if buf is None:
        return None
    magic, version, kind, module, method, request_id, total = struct.unpack(">HBBHHQI", buf[:20])
    assert magic == MAGIC, "bad magic: 0x%04X" % magic
    assert version == VERSION, "bad version: %d" % version
    assert HEADER_LEN <= total <= 65536
    body = read_exact(sock, total - HEADER_LEN)
    return None if body is None else (kind, module, method, request_id, body)


def read_exact(sock, count):
    buf = b""
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def walk_req(sx, sy, dx, dy):
    # gs.WalkReq { int32 sx=1; sy=2; dx=3; dy=4; }：字段号<<3 | wiretype(0=varint)
    return bytes([0x08, sx, 0x10, sy, 0x18, dx, 0x20, dy])


def main():
    port = int(sys.argv[1])
    player_id = int(sys.argv[2])
    ticket = int(sys.argv[3])

    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.settimeout(5)

    # ---- 1. 鉴权（body: playerId(u64 LE) + ticket(u64 LE)）----
    auth_body = struct.pack("<QQ", player_id, ticket)
    sock.sendall(client_frame(KIND_CONTROL, SYS_MODULE, SYS_AUTH, 1, auth_body))
    f = read_frame(sock)
    if f is None:
        print("FAIL: auth 阶段连接被关闭")
        return 1
    kind, module, method, request_id, body = f
    print("auth: kind=%d method=%d requestId=%d bodyLen=%d" % (kind, method, request_id, len(body)))
    if kind != KIND_CONTROL or method != SYS_AUTH_ACK:
        print("FAIL: 期待 AuthAck(kind=5,method=2)，实际 kind=%d method=%d" % (kind, method))
        return 1
    if len(body) >= 17:
        pid, sid, epoch = struct.unpack("<QQI", body[:20])
        print("auth ok: playerId=%d sessionId=%d epoch=%d dstService=%d" % (pid, sid, epoch, body[24]))

    # 鉴权只建立会话；显式场景登录成功后才允许移动。
    sock.sendall(client_frame(KIND_REQUEST, MOD_PLAYER, 4, 10))
    f = read_frame(sock)
    if f and f[0] == KIND_ERROR and b"unavailable" in f[4]:
        sock.close()
        return 2
    if not f or f[:4] != (KIND_RESPONSE, MOD_PLAYER, 4, 10) or not f[4] or f[4][0] != 0x10:
        raise AssertionError("scene login failed: %r" % (f,))
    print("enter scene ok: body=" + f[4].hex())

    # ---- 2. 真实业务请求：PLAYER/PLAYER_MOVE + gs.WalkReq ----
    sock.sendall(client_frame(KIND_REQUEST, MOD_PLAYER, METHOD_MOVE, 2, walk_req(0, 0, 10, 20)))

    # 3 秒内可能先收到推送（Lua 定时器/状态推送），把业务响应挑出来
    for _ in range(8):
        f = read_frame(sock)
        if f is None:
            print("FAIL: 等待响应时连接被关闭")
            return 1
        kind, module, method, request_id, body = f
        print("recv: kind=%d module=%d method=%d requestId=%d bodyLen=%d body=%s"
              % (kind, module, method, request_id, len(body), body.hex()))
        if request_id == 2:
            if kind == KIND_ERROR and b"unavailable" in body:
                # 后端（场景服）还没注册完：这是"还没就绪"，不是失败。
                # 外层脚本据此重试（比"等日志落盘"确定得多）。
                print("NOT_READY: 网关还没有可用的后端服务")
                return 2
            if kind != KIND_RESPONSE:
                print("FAIL: 期待 Response(kind=2)，实际 kind=%d" % kind)
                return 1
            if (module, method, body) != (MOD_PLAYER, METHOD_MOVE, bytes.fromhex("100a1814")):
                print("FAIL: WalkAck 内容或响应头错误")
                return 1
            print("response ok: module=%d method=%d body=%s" % (module, method, body.hex()))
            break
    else:
        print("FAIL: 没等到 requestId=2 的响应")
        return 1

    sock.sendall(client_frame(KIND_REQUEST,MOD_PLAYER,METHOD_MOVE,20,walk_req(0,0,11,20)))
    f=read_frame(sock)
    assert f and f[:4] == (KIND_RESPONSE,MOD_PLAYER,METHOD_MOVE,20) and b"position mismatch" in f[4], f
    sock.sendall(client_frame(KIND_REQUEST,MOD_PLAYER,METHOD_MOVE,21,walk_req(10,20,11,20)))
    f=read_frame(sock)
    assert f == (KIND_RESPONSE,MOD_PLAYER,METHOD_MOVE,21,bytes.fromhex('100b1814')), f
    print("invalid move ok: error preserves route and authoritative position")

    second=socket.create_connection(('127.0.0.1',port),timeout=5)
    second.settimeout(5)
    second.sendall(client_frame(KIND_CONTROL,SYS_MODULE,SYS_AUTH,1,struct.pack('<QQ',player_id+1,ticket)))
    assert read_frame(second)[0] == KIND_CONTROL
    second.sendall(client_frame(KIND_REQUEST,MOD_PLAYER,4,2))
    f=read_frame(second)
    assert f and f[:4] == (KIND_RESPONSE,MOD_PLAYER,4,2) and f[4][0] == 0x10, f
    second.close()
    # 同一后端网关连接复用的另一个玩家断开，不应影响第一个玩家。
    sock.sendall(client_frame(KIND_REQUEST,MOD_PLAYER,METHOD_MOVE,22,walk_req(11,20,12,20)))
    assert read_frame(sock) == (KIND_RESPONSE,MOD_PLAYER,METHOD_MOVE,22,bytes.fromhex('100c1814'))
    print("multiplex ok: another client disconnect does not remove this player")

    # RetTip {code=1001, text="route not found"}，响应仍必须沿用无效请求的头。
    tip = bytes.fromhex("08e907120f") + b"route not found"
    for request_id, invalid_module, invalid_method in ((3, 9999, 9999), (4, MOD_PLAYER, 9999)):
        sock.sendall(client_frame(KIND_REQUEST, invalid_module, invalid_method, request_id))
        for _ in range(8):
            f = read_frame(sock)
            if f is None:
                print("FAIL: 无效路由回包前连接被关闭")
                return 1
            kind, module, method, rid, body = f
            if rid == request_id:
                if (kind, module, method, body) != (KIND_RESPONSE, invalid_module, invalid_method, tip):
                    print("FAIL: 无效路由的响应头或 RetTip 内容错误: %s" % (f,))
                    return 1
                print("invalid route ok: module=%d method=%d requestId=%d body=%s" %
                      (module, method, rid, body.hex()))
                break
        else:
            print("FAIL: 没等到无效路由的响应")
            return 1

    # 主动退出之后不能再移动，不能被每请求的幂等 ONLINE 复活。
    sock.sendall(client_frame(KIND_REQUEST, MOD_PLAYER, 5, 11))
    f = read_frame(sock)
    assert f == (KIND_RESPONSE, MOD_PLAYER, 5, 11, b""), f
    sock.sendall(client_frame(KIND_REQUEST, MOD_PLAYER, METHOD_MOVE, 12, walk_req(10,20,11,20)))
    f = read_frame(sock)
    assert f and f[:4] == (KIND_RESPONSE,MOD_PLAYER,METHOD_MOVE,12) and b"not online" in f[4], f
    print("logout ok: subsequent move rejected")
    # 再鉴权产生新 epoch，重新进场；最终由客户端断开触发 OFFLINE。
    sock.sendall(client_frame(KIND_CONTROL,SYS_MODULE,SYS_AUTH,13,auth_body))
    f = read_frame(sock)
    assert f and f[:4] == (KIND_CONTROL,SYS_MODULE,SYS_AUTH_ACK,13), f
    sock.sendall(client_frame(KIND_REQUEST,MOD_PLAYER,4,14))
    f = read_frame(sock)
    assert f and f[:4] == (KIND_RESPONSE,MOD_PLAYER,4,14) and f[4][0] == 0x10, f
    sock.close()
    print("disconnect sent: final client session closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
