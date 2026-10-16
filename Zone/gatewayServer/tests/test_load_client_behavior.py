#!/usr/bin/env python3
"""Black-box signal/drain and JSON accounting checks for gateway_load_client."""
import json
import pathlib
import socket
import select
import struct
import subprocess
import threading
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
CLIENT = pathlib.Path(__import__("os").environ.get("GATEWAY_LOAD_CLIENT", ""))


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise EOFError
        data.extend(chunk)
    return bytes(data)


def read_frame(conn):
    header = recv_exact(conn, 32)
    total = struct.unpack("!I", header[16:20])[0]
    return header, recv_exact(conn, total - 32)


def frame(kind, module, method, request_id, body=b""):
    return struct.pack("!HBBHHQI", 0xC1EB, 2, kind, module, method, request_id, 32 + len(body)) + bytes(12) + body


class LoadClientBehaviorTests(unittest.TestCase):
    def test_sigterm_stops_sends_drains_delayed_response_and_writes_json(self):
        self.assertTrue(CLIENT.is_file(), "set GATEWAY_LOAD_CLIENT to the built load client")
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        port = listener.getsockname()[1]
        done = threading.Event()
        auth_ready = threading.Event()
        ping_seen = threading.Event()
        response_sent = threading.Event()
        proc_ref = [None]
        stop_requested = threading.Event()
        accepted_requests = []

        def server():
            try:
                conn, _ = listener.accept()
                with conn:
                    read_frame(conn)  # Auth
                    conn.sendall(frame(5, 1, 2, 1, struct.pack("<QQIIB", 100000, 7, 1, 1, 8)))
                    auth_ready.set()
                    conn.settimeout(2)
                    while True:
                        header, _ = read_frame(conn)
                        method = struct.unpack("!H", header[6:8])[0]
                        if method == 6:
                            accepted_requests.append(header)
                            ping_seen.set()
                            ready, _, _ = select.select([conn], [], [], 0.05)
                            if ready:
                                try:
                                    header, _ = read_frame(conn)
                                    if struct.unpack("!H", header[6:8])[0] == 6:
                                        accepted_requests.append(header)
                                except (OSError, EOFError, socket.timeout):
                                    pass
                            break
                    stop_requested.wait(0.1)
                    for request in accepted_requests:
                        conn.sendall(frame(5, 1, 7, struct.unpack("!Q", request[8:16])[0]))
                    conn.settimeout(0.1)
                    try:
                        conn.recv(4096)  # discard the next scheduled request before it is admitted by the client
                    except socket.timeout:
                        pass
                    response_sent.set()
                    done.set()
            except (OSError, EOFError):
                pass
            finally:
                listener.close()

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        with __import__("tempfile").TemporaryDirectory() as tmp:
            output = pathlib.Path(tmp) / "summary.json"
            proc = subprocess.Popen([str(CLIENT), "--host", "127.0.0.1", "--port", str(port), "--connections", "1",
                                     "--rps", "1", "--duration", "30", "--drain", "2",
                                     "--ticket", "998877", "--player-base", "100000",
                                     "--output", str(output)], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            proc_ref[0] = proc
            self.assertTrue(auth_ready.wait(2))
            self.assertTrue(ping_seen.wait(2))
            stop_requested.set()
            time.sleep(0.02)
            proc.terminate()
            stdout, _ = proc.communicate(timeout=5)
            response_sent.wait(1)
            thread.join(timeout=2)
            self.assertTrue(done.is_set(), stdout)
            result = json.loads(output.read_text())
            self.assertGreaterEqual(result["load_attempted"], 1)
            self.assertGreaterEqual(result["completed"], 1)
            self.assertEqual(result["load_attempted"], result["completed"] + result["unresolved"])
            self.assertLessEqual(result["drain_final_unfinished"], result["cutoff_unfinished"])
            self.assertEqual(result["in_flight"], 0)
            self.assertTrue(response_sent.is_set())


if __name__ == "__main__":
    unittest.main()