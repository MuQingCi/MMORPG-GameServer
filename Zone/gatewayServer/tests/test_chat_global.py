"""单网关真实进程冒烟：两客户端私聊和全局服务 ping。仅使用 Python 标准库。"""
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
    head = read_exact(sock, 20)
    magic, version, kind, module, method, request, length = struct.unpack("!HBBHHQI", head)
    assert magic == 0xC1EB and version == 1 and 20 <= length <= 65536
    return kind, module, method, request, read_exact(sock, length - 20)


def send(sock, kind, module, method, request, body=b""):
    sock.sendall(struct.pack("!HBBHHQI", 0xC1EB, 1, kind, module, method,
                             request, 20 + len(body)) + body)


def auth(port, player):
    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
    sock.settimeout(2)
    send(sock, 5, 1, 1, 1, struct.pack("<QQ", player, 998877))
    kind, module, method, request, body = receive(sock)
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
            send(a, 1, 100, 1, 10, struct.pack("!Q", 1002) + b"hello")
            assert receive(a) == (2, 100, 1, 10, b"accepted")
            assert receive(b) == (3, 100, 1, 0, struct.pack("!Q", 1001) + b"hello")
            send(a, 1, 100, 1, 11, b"bad")
            assert receive(a) == (2, 100, 1, 11, b"invalid chat message")
            send(a, 1, 101, 1, 12, b"global-echo")
            assert receive(a) == (2, 101, 1, 12, b"global-echo")
            print("OK: chat ack/push/invalid and global echo")
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