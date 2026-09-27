#!/usr/bin/env python3
# ============================================================================
# 真实链路客户端（网关公网端口）：只依赖标准库，不链接项目代码
#
# 做的事：
#   1. 连网关公网端口，发 ClientFrame 鉴权（playerId + ticket）-> 期待 AuthAck
#   2. 发一条真实业务请求 PLAYER(3)/MOVE(2)，body 是手写的 gs.WalkReq
#      （proto3 小字段号 + 小整数 = 全 varint，手写字节即可，不必引入 protobuf 运行时）
#   3. 读出网关回来的 ClientFrame，把 kind/module/method/requestId/body(hex) 打出来
#      （断言交给外层脚本：body 是否解出 WalkAck 的字段）
#
# 用法: real_scene_client.py <publicPort> <playerId> <ticket>
# ============================================================================
import socket
import struct
import sys

MAGIC = 0xC1EB
VERSION = 1
HEADER_LEN = 20

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
    return struct.pack(">HBBHHQI", MAGIC, VERSION, kind, module, method, request_id, total) + body


def read_frame(sock):
    buf = b""
    while len(buf) < HEADER_LEN:
        chunk = sock.recv(4096)
        if not chunk:
            return None
        buf += chunk
    magic, version, kind, module, method, request_id, total = struct.unpack(">HBBHHQI", buf[:HEADER_LEN])
    assert magic == MAGIC, "bad magic: 0x%04X" % magic
    assert version == VERSION, "bad version: %d" % version
    while len(buf) < total:
        chunk = sock.recv(4096)
        if not chunk:
            return None
        buf += chunk
    return (kind, module, method, request_id, buf[HEADER_LEN:total])


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
            print("response ok: module=%d method=%d body=%s" % (module, method, body.hex()))
            return 0

    print("FAIL: 没等到 requestId=2 的响应")
    return 1


if __name__ == "__main__":
    sys.exit(main())
