"""真实进程冒烟：单/双网关私聊、区服喊话及全局服务 ping。仅使用标准库。"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def read_exact(sock, count):
    result = b""
    while len(result) < count:
        part = sock.recv(count - len(result))
        if not part:
            raise RuntimeError("socket closed while reading frame")
        result += part
    return result


def receive(sock):
    head = read_exact(sock, 32)
    magic, version, kind, module, method, request, length = struct.unpack("!HBBHHQI", head[:20])
    assert magic == 0xC1EB and version == 2 and 32 <= length <= 65536
    role = head[20:32].rstrip(b"\0").decode("utf8")
    return kind, module, method, request, role, read_exact(sock, length - 32)


def send(sock, kind, module, method, request, body=b"", role=b""):
    assert len(role) <= 12
    sock.sendall(struct.pack("!HBBHHQI", 0xC1EB, 2, kind, module, method,
                             request, 32 + len(body)) + role.ljust(12, b"\0") + body)


def auth(port, player):
    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
    sock.settimeout(2)
    send(sock, 5, 1, 1, 1, struct.pack("<QQ", player, 998877))
    kind, module, method, request, role, body = receive(sock)
    assert (kind, module, method, request) == (5, 1, 2, 1)
    assert struct.unpack_from("<Q", body)[0] == player
    return sock


def main():
    gateway, chat, global_server = sys.argv[1:4]
    public, private = free_port(), free_port()
    while private == public:
        private = free_port()
    with tempfile.TemporaryDirectory(prefix="chat-global-") as temp:
        config = os.path.join(temp, "gateway.yaml")
        with open(config, "w", encoding="utf8") as fp:
            fp.write(f"""gateway:
    zoneId: 1
    gatewayId: 1
    publicConfig:
        listenAddr: 127.0.0.1
        listenPort: {public}
    privateConfig:
        listenAddr: 127.0.0.1
        listenPort: {private}
        allowedServices:
            - 3
            - 4
            - 8
    eventThreadNum: 2
    clientToken: 998877
    defaultServiceId: 8
    maxClientSessions: 32
    maxPendingRoutes: 128
    routeTtlMs: 5000
""")
        processes = []
        clients = []
        try:
            for command in ([gateway, config], [chat, "127.0.0.1", str(private), "1"],
                            [global_server, "127.0.0.1", str(private), "1"]):
                if len(processes) == 0:
                    processes.append(subprocess.Popen(command, cwd=temp, stdout=subprocess.DEVNULL,
                                                     stderr=subprocess.PIPE))
                    deadline = time.monotonic() + 5
                    while True:
                        try:
                            with socket.create_connection(("127.0.0.1", private), timeout=0.1):
                                break
                        except OSError:
                            if time.monotonic() > deadline:
                                raise RuntimeError("gateway did not listen")
                            time.sleep(0.03)
                else:
                    processes.append(subprocess.Popen(command, cwd=temp, stdout=subprocess.DEVNULL,
                                                     stderr=subprocess.PIPE))
            time.sleep(0.35)  # 等待两个服务完成握手注册
            a, b = auth(public, 1001), auth(public, 1002)
            clients.extend((a, b))
            send(a, 1, 100, 1, 10, struct.pack("!Q", 1002) + b"hello", b"Alice")
            assert receive(a) == (2, 100, 1, 10, "", b"accepted")
            assert receive(b) == (3, 100, 1, 0, "Alice", struct.pack("!Q", 1001) + b"hello")
            send(a, 1, 100, 1, 11, b"bad", b"Alice")
            assert receive(a) == (4, 1, 1, 11, "", b"invalid chat message")
            send(a, 1, 100, 1, 13, struct.pack("!Q", 1002) + b"hello", "中文".encode())
            assert receive(a) == (2, 100, 1, 13, "", b"accepted")
            assert receive(b) == (3, 100, 1, 0, "中文", struct.pack("!Q", 1001) + b"hello")
            send(a, 1, 100, 1, 15, struct.pack("!Q", 1002) + b"full", b"123456789012")
            assert receive(a) == (2, 100, 1, 15, "", b"accepted")
            assert receive(b) == (3, 100, 1, 0, "123456789012", struct.pack("!Q", 1001) + b"full")
            max_text = b"x" * 1024
            send(a, 1, 100, 1, 16, struct.pack("!Q", 1002) + max_text, b"Alice")
            assert receive(a) == (2, 100, 1, 16, "", b"accepted")
            assert receive(b) == (3, 100, 1, 0, "Alice", struct.pack("!Q", 1001) + max_text)
            # 单网关没有离线投递：入队成功不等于目标玩家收到消息。
            send(a, 1, 100, 1, 17, struct.pack("!Q", 9999) + b"offline", b"Alice")
            assert receive(a) == (2, 100, 1, 17, "", b"accepted")
            send(a, 1, 100, 1, 14, struct.pack("!Q", 1002) + b"hello")
            assert receive(a) == (4, 1, 1, 14, "", b"chat role name required")
            invalid_chats = (
                (0, b"hello"),          # 不存在的目标 ID
                (1001, b"hello"),       # 不允许给自己发私聊
                (1002, b""),            # 空文本
                (1002, b"x" * 1025),    # 超过业务文本限制
                (1002, b"\xff"),        # 非法 UTF-8
                (1002, b"a\0b"),       # 内嵌控制字符
            )
            for request, (target, text) in enumerate(invalid_chats, start=20):
                send(a, 1, 100, 1, request, struct.pack("!Q", target) + text, b"Alice")
                assert receive(a) == (2, 100, 1, request, "", b"invalid chat message")
            b.settimeout(0.15)
            try:
                try:
                    receive(b)
                    raise AssertionError("invalid chat message was pushed")
                except socket.timeout:
                    pass
            finally:
                b.settimeout(2)
            send(a, 1, 100, 99, 30, b"unsupported", b"Alice")
            assert receive(a) == (2, 100, 99, 30, "", b"unsupported method")
            send(a, 1, 101, 1, 12, b"global-echo")
            assert receive(a) == (2, 101, 1, 12, "", b"global-echo")
            send(a, 1, 101, 1, 33)
            assert receive(a) == (2, 101, 1, 33, "", b"")
            send(b, 1, 101, 1, 31, b"\0\xff\x00global")
            assert receive(b) == (2, 101, 1, 31, "", b"\0\xff\x00global")
            send(a, 1, 101, 99, 32, b"bad method")
            assert receive(a) == (2, 101, 99, 32, "", b"unsupported method")
            print("OK: chat ack/push/invalid/unsupported and global echo/binary/unsupported")
        finally:
            for client in clients:
                client.close()
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                try:
                    process.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()


def multi_gateway(gateway, chat):
    ports = []
    while len(ports) < 4:
        port = free_port()
        if port not in ports:
            ports.append(port)
    with tempfile.TemporaryDirectory(prefix="chat-multi-") as temp:
        processes, clients = [], []
        try:
            for i in range(2):
                config = os.path.join(temp, f"gateway{i}.yaml")
                with open(config, "w", encoding="utf8") as fp:
                    fp.write(f"""gateway:
    zoneId: 1
    gatewayId: {i + 1}
    publicConfig:
        listenAddr: 127.0.0.1
        listenPort: {ports[2 * i]}
    privateConfig:
        listenAddr: 127.0.0.1
        listenPort: {ports[2 * i + 1]}
        allowedServices:
            - 3
            - 8
    eventThreadNum: 2
    clientToken: 998877
    defaultServiceId: 8
    maxClientSessions: 32
    maxPendingRoutes: 128
    routeTtlMs: 5000
""")
                processes.append(subprocess.Popen([gateway, config], cwd=temp,
                                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))
            for port in (ports[1], ports[3]):
                deadline = time.monotonic() + 5
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise RuntimeError("gateway did not listen")
                        time.sleep(0.03)
            processes.append(subprocess.Popen([chat, "127.0.0.1", str(ports[1]), "1",
                                               "127.0.0.1", str(ports[3]), "1"], cwd=temp,
                                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))
            time.sleep(0.45)
            a, b, c = auth(ports[0], 2001), auth(ports[2], 2002), auth(ports[0], 2003)
            clients.extend((a, b, c))
            unauth = socket.create_connection(("127.0.0.1", ports[2]), timeout=2)
            unauth.settimeout(0.2)
            clients.append(unauth)
            send(a, 1, 100, 1, 41, struct.pack("!Q", 2002) + b"cross", b"Alice")
            assert receive(a) == (2, 100, 1, 41, "", b"accepted")
            assert receive(b) == (3, 100, 1, 0, "Alice", struct.pack("!Q", 2001) + b"cross")
            send(b, 1, 100, 1, 42, struct.pack("!Q", 2003) + b"return", b"Bob")
            assert receive(b) == (2, 100, 1, 42, "", b"accepted")
            assert receive(c) == (3, 100, 1, 0, "Bob", struct.pack("!Q", 2002) + b"return")
            send(a, 1, 100, 2, 43, b"zone-wide", b"Alice")
            assert receive(a) == (2, 100, 2, 43, "", b"accepted")
            expected = (3, 100, 2, 0, "Alice", struct.pack("!Q", 2001) + b"zone-wide")
            assert receive(a) == expected
            assert receive(b) == expected
            assert receive(c) == expected
            for request, invalid_text in ((44, b"\xff"), (46, b""), (47, b"x" * 1025)):
                send(b, 1, 100, 2, request, invalid_text, b"Bob")
                assert receive(b) == (2, 100, 2, request, "", b"invalid chat message")
            send(a, 1, 100, 3, 45, b"fake admin", b"Alice")
            assert receive(a) == (2, 100, 3, 45, "", b"admin broadcast unavailable")
            for sock in (a, b, c, unauth):
                sock.settimeout(0.2)
                try:
                    receive(sock)
                    raise AssertionError("duplicate or unauthorized broadcast push")
                except socket.timeout:
                    pass
            print("OK: two gateways private chat, shout fanout and admin denial")
        finally:
            for client in clients:
                client.close()
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                try:
                    process.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()


if __name__ == "__main__":
    main()
    multi_gateway(sys.argv[1], sys.argv[2])