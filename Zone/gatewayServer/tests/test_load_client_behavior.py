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
    def test_scene_fails_when_logout_ack_is_business_error(self):
        self.assertTrue(CLIENT.is_file())
        listener = socket.socket(); listener.bind(("127.0.0.1", 0)); listener.listen()
        port = listener.getsockname()[1]
        def server():
            conn, _ = listener.accept()
            with conn:
                read_frame(conn)
                conn.sendall(frame(5, 1, 2, 1, struct.pack('<QQIIB', 100000, 7, 1, 1, 8)))
                read_frame(conn)
                conn.sendall(frame(2, 3, 4, 2, bytes([16, 1, 32, 10, 40, 20])))
                time.sleep(0.5)
                h, _ = read_frame(conn)
                rid = struct.unpack('!Q', h[8:16])[0]
                conn.sendall(frame(2, 3, 2, rid, bytes([16, 10, 24, 20])))
                h, _ = read_frame(conn)
                conn.sendall(frame(2, 3, 5, struct.unpack('!Q', h[8:16])[0], bytes([8, 1])))
            listener.close()
        thread = threading.Thread(target=server, daemon=True); thread.start()
        with __import__('tempfile').TemporaryDirectory() as tmp:
            barrier = pathlib.Path(tmp) / 'barrier'; barrier.mkdir()
            output = pathlib.Path(tmp) / 'load.json'
            proc = subprocess.Popen([str(CLIENT), '--host', '127.0.0.1', '--port', str(port), '--connections', '1',
                                     '--rps', '1', '--duration', '1', '--drain', '2', '--scene', '--barrier-dir', str(barrier),
                                     '--player-base', '100000', '--output', str(output)], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            ready = barrier / 'load.json.0.ready'; deadline = time.time() + 5
            while time.time() < deadline and not ready.exists(): time.sleep(.02)
            self.assertTrue(ready.exists()); time.sleep(.25); (barrier / 'START').touch()
            stdout, _ = proc.communicate(timeout=8); thread.join(timeout=2)
            self.assertNotEqual(proc.returncode, 0, stdout)
            result = json.loads(output.read_text())
            self.assertEqual(result['logout_failed'], 1)

    def test_scene_prepares_once_waits_for_global_start_and_logs_out_once(self):
        self.assertTrue(CLIENT.is_file(), "set GATEWAY_LOAD_CLIENT to the built load client")
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        port = listener.getsockname()[1]
        events = []
        ready = None
        entered = threading.Event()

        def server():
            conn, _ = listener.accept()
            with conn:
                h, _ = read_frame(conn); events.append((h[2], struct.unpack('!H', h[4:6])[0]))
                conn.sendall(frame(5, 1, 2, 1, struct.pack('<QQIIB', 100000, 7, 1, 1, 8)))
                h, _ = read_frame(conn); events.append((h[2], struct.unpack('!H', h[6:8])[0]))
                conn.sendall(frame(2, 3, 4, 2, bytes([16, 1, 32, 10, 40, 20])))
                entered.set()
                conn.settimeout(0.25)
                try:
                    early = conn.recv(1)
                    if early:
                        raise AssertionError("client sent load before START")
                except socket.timeout:
                    pass
                conn.settimeout(5)
                h, body = read_frame(conn)
                method = struct.unpack('!H', h[6:8])[0]
                events.append((h[2], method))
                self.assertEqual(method, 2)
                conn.sendall(frame(2, 3, 2, struct.unpack('!Q', h[8:16])[0], bytes([16, 10, 24, 20])))
                h, _ = read_frame(conn); events.append((h[2], struct.unpack('!H', h[6:8])[0]))
                rid = struct.unpack('!Q', h[8:16])[0]
                conn.sendall(frame(2, 3, 5, rid, b''))
            listener.close()

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        with __import__("tempfile").TemporaryDirectory() as tmp:
            barrier = pathlib.Path(tmp) / "barrier"
            barrier.mkdir()
            output = pathlib.Path(tmp) / "load-gw1.json"
            ready = barrier / "load-gw1.json.0.ready"
            proc = subprocess.Popen([str(CLIENT), "--host", "127.0.0.1", "--port", str(port), "--connections", "1",
                                     "--rps", "1", "--duration", "1", "--drain", "2", "--scene",
                                     "--metrics-output", str(pathlib.Path(tmp) / "metrics.jsonl"),
                                     "--barrier-dir", str(barrier), "--player-base", "100000", "--output", str(output)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            deadline = time.time() + 5
            while time.time() < deadline and not ready.exists(): time.sleep(0.02)
            self.assertTrue(ready.exists())
            self.assertTrue(entered.wait(2))
            time.sleep(0.3)
            (barrier / "START").touch()
            stdout, _ = proc.communicate(timeout=8)
            thread.join(timeout=3)
            self.assertEqual(proc.returncode, 0, stdout)
            result = json.loads(output.read_text())
            self.assertEqual(result["logout_succeeded"], 1)
            self.assertEqual(result["logout_failed"], 0)
            positions = [json.loads(line) for line in pathlib.Path(str(output) + ".players.jsonl").read_text().splitlines()]
            self.assertEqual(len(positions), 1)
            self.assertEqual(positions[0]["playerId"], 100000)
            self.assertEqual((positions[0]["x"], positions[0]["y"]), (10, 20))
            self.assertEqual(positions[0]["successfulMoves"], 1)
            samples = [json.loads(line) for line in (pathlib.Path(tmp) / "metrics.jsonl").read_text().splitlines()]
            self.assertEqual(samples[0]["elapsedMs"], 0)
            self.assertEqual(samples[-1]["elapsedMs"], 3000)
            self.assertTrue(samples[-1]["finalSample"])
            methods = [method for _, method in events]
            self.assertEqual(methods.count(4), 1)
            self.assertEqual(methods.count(2), 1)
            self.assertEqual(methods.count(5), 1)

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