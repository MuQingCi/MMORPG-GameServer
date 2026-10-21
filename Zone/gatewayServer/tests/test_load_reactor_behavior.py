#!/usr/bin/env python3
"""少连接事件驱动黑盒验证，不是容量压测；假服务使用单个 selector 线程。"""
import json
import os
import pathlib
import selectors
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

from test_load_client_behavior import frame

CLIENT = os.environ.get('GATEWAY_LOAD_CLIENT', '')


class FakeGateway:
    def __init__(self, mode='normal'):
        self.mode = mode
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(128)
        self.listener.setblocking(False)
        self.port = self.listener.getsockname()[1]
        self.identities = set()
        self.requests = {}
        self.stopped = threading.Event()
        self.errors = []
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *unused):
        self.stopped.set()
        self.thread.join(2)
        self.listener.close()

    def run(self):
        selector = selectors.DefaultSelector()
        selector.register(self.listener, selectors.EVENT_READ, None)
        try:
            while not self.stopped.is_set():
                for key, events in selector.select(.005):
                    sock, state = key.fileobj, key.data
                    if state is None:
                        conn, _ = sock.accept()
                        conn.setblocking(False)
                        selector.register(conn, selectors.EVENT_READ,
                                          dict(input=bytearray(), output=bytearray(), player=0))
                        continue
                    try:
                        if events & selectors.EVENT_READ:
                            data = sock.recv(65536)
                            if not data:
                                selector.unregister(sock)
                                sock.close()
                                continue
                            state['input'].extend(data)
                            while len(state['input']) >= 32:
                                size = struct.unpack('!I', state['input'][16:20])[0]
                                if len(state['input']) < size:
                                    break
                                packet = bytes(state['input'][:size])
                                del state['input'][:size]
                                kind, module, method, rid = struct.unpack('!BHHQ', packet[3:16])
                                if method == 1 and module == 1:
                                    player = struct.unpack('<Q', packet[32:40])[0]
                                    state['player'] = player
                                    self.identities.add(player)
                                    if self.mode != 'auth-stall':
                                        state['output'].extend(frame(5, 1, 2, rid,
                                            struct.pack('<QQIIB', player + (self.mode == 'auth-wrong'), 7, 1, 1, 8)))
                                elif method == 4 and module == 3:
                                    state['output'].extend(frame(2, 3, 4, rid, bytes([16, 1, 32, 10, 40, 20])))
                                elif method == 5 and module == 3:
                                    state['output'].extend(frame(2, 3, 5, rid))
                                else:
                                    player = state['player']
                                    self.requests[player] = self.requests.get(player, 0) + 1
                                    if self.mode == 'slow' and player == 100000:
                                        continue
                                    if self.mode == 'close':
                                        selector.unregister(sock)
                                        sock.close()
                                        break
                                    if self.mode == 'wrong-id':
                                        rid += 1000
                                    body = bytes([8, 1, 16, 10, 24, 20]) if self.mode == 'business-error' else bytes([16, 10, 24, 20])
                                    response = frame(5, 1, 7, rid) if module == 1 else frame(2, module, method, rid, body)
                                    # 粘包推送不能被误当成请求响应。
                                    pushes = 140 if self.mode == 'flood' else 1
                                    state['output'].extend(frame(3, 100, 1, 0) * pushes + response)
                        if sock.fileno() < 0:
                            continue
                        if events & selectors.EVENT_WRITE and state['output']:
                            # 每次只发送 7 字节，强制覆盖帧头和帧体分片。
                            n = sock.send(state['output'][:65536 if self.mode == 'flood' else 7])
                            del state['output'][:n]
                        selector.modify(sock, selectors.EVENT_READ |
                                        (selectors.EVENT_WRITE if state['output'] else 0), state)
                    except (ConnectionError, OSError):
                        try:
                            selector.unregister(sock)
                        except (KeyError, ValueError):
                            pass
                        sock.close()
        except Exception as exc:
            self.errors.append(repr(exc))
        finally:
            for key in list(selector.get_map().values()):
                if key.data is not None:
                    key.fileobj.close()
            selector.close()


class ReactorBehaviorTests(unittest.TestCase):
    def test_idle_and_hold_keep_business_inside_formal_window(self):
        with FakeGateway() as server, tempfile.TemporaryDirectory() as tmp:
            output = pathlib.Path(tmp) / 'load.json'
            begin = time.monotonic()
            proc = subprocess.Popen([CLIENT, '--port', str(server.port), '--scene',
                '--duration', '1', '--rps', '2', '--idle-before', '1', '--hold-after', '2',
                '--output', str(output)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                time.sleep(.6)
                self.assertIsNone(proc.poll())
                self.assertFalse(server.requests)
                time.sleep(1.9)
                self.assertIsNone(proc.poll())
                sent = sum(server.requests.values())
                self.assertGreater(sent, 0)
                time.sleep(.5)
                self.assertEqual(sum(server.requests.values()), sent)
                stdout, _ = proc.communicate(timeout=4)
                self.assertEqual(proc.returncode, 0, stdout)
                self.assertGreaterEqual(time.monotonic() - begin, 4)
                self.assertEqual(json.loads(output.read_text())['load_attempted'], sent)
                phases = [json.loads(line) for line in pathlib.Path(str(output) + '.phases.jsonl').read_text().splitlines()]
                self.assertEqual([p['phase'] for p in phases], ['idle', 'load', 'drain', 'hold', 'logout'])
                self.assertGreaterEqual(phases[1]['monotonicMs'] - phases[0]['monotonicMs'], 990)
                self.assertGreaterEqual(phases[-1]['monotonicMs'] - phases[-2]['monotonicMs'], 1990)
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.communicate()

    def test_invalid_phase_durations_are_rejected(self):
        for flag in ('--idle-before', '--hold-after'):
            for value in ('-1', '301', 'abc'):
                result = subprocess.run([CLIENT, flag, value], capture_output=True, timeout=2)
                self.assertEqual(result.returncode, 2)

    def test_signal_during_idle_and_hold_exits_promptly(self):
        for delay in (.5, 1.5):
            with self.subTest(delay=delay), FakeGateway() as server, tempfile.TemporaryDirectory() as tmp:
                proc = subprocess.Popen([CLIENT, '--port', str(server.port), '--scene', '--duration', '1',
                    '--idle-before', '20' if delay < 1 else '0', '--hold-after', '20',
                    '--output', str(pathlib.Path(tmp) / 'load.json')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                try:
                    time.sleep(delay)
                    proc.send_signal(signal.SIGTERM)
                    proc.communicate(timeout=2)
                finally:
                    if proc.poll() is None:
                        proc.kill()
                    proc.communicate()

    def test_invalid_io_thread_counts_are_rejected(self):
        for value in ('0', '-1', '4294967296', 'abc'):
            result = subprocess.run([CLIENT, '--io-threads', value], capture_output=True, timeout=2)
            self.assertEqual(result.returncode, 2, value)

    def test_absent_start_abort_and_signal_do_not_hang_sampler(self):
        for action in ('timeout', 'abort', 'signal', 'invalid-start', 'future-start'):
            with self.subTest(action=action), FakeGateway() as server, tempfile.TemporaryDirectory() as tmp:
                directory = pathlib.Path(tmp)
                output = directory / 'load.json'
                proc = subprocess.Popen([CLIENT, '--port', str(server.port), '--connections', '2',
                    '--duration', '30', '--preparation-timeout', '1', '--barrier-dir', tmp,
                    '--metrics-output', str(directory / 'metrics.jsonl'), '--output', str(output)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                try:
                    deadline = time.monotonic() + 2
                    while not (directory / 'load.json.1.ready').exists() and proc.poll() is None and time.monotonic() < deadline:
                        time.sleep(.01)
                    self.assertTrue((directory / 'load.json.1.ready').exists())
                    self.assertFalse(server.requests)
                    if action == 'abort':
                        (directory / 'ABORT').touch()
                    elif action == 'signal':
                        proc.send_signal(signal.SIGTERM)
                    elif action == 'invalid-start':
                        (directory / 'START').write_text('not-a-timestamp')
                    elif action == 'future-start':
                        (directory / 'START').write_text('9223372036854775807')
                    stdout, _ = proc.communicate(timeout=3)
                    self.assertNotEqual(proc.returncode, 0, stdout)
                    result = json.loads(output.read_text())
                    self.assertEqual(result['load_attempted'], 0)
                finally:
                    if proc.poll() is None:
                        proc.kill()
                        proc.communicate()

    def test_authentication_failure_and_stall_exit_without_business_attempt(self):
        for mode in ('auth-stall', 'auth-wrong'):
            with self.subTest(mode=mode), FakeGateway(mode) as server, tempfile.TemporaryDirectory() as tmp:
                output = pathlib.Path(tmp) / 'load.json'
                proc = subprocess.run([CLIENT, '--port', str(server.port), '--connections', '2',
                    '--preparation-timeout', '1', '--duration', '30',
                    '--metrics-output', str(pathlib.Path(tmp) / 'metrics.jsonl'), '--output', str(output)],
                    capture_output=True, text=True, timeout=4)
                self.assertNotEqual(proc.returncode, 0)
                result = json.loads(output.read_text())
                self.assertEqual(result['load_attempted'], 0)
                self.assertEqual(result['completed'], 0)
                self.assertEqual(result['in_flight'], 0)

    def test_aoi_final_barrier_keeps_connection_until_observer_evidence(self):
        with FakeGateway() as server, tempfile.TemporaryDirectory() as tmp:
            directory = pathlib.Path(tmp)
            barrier = directory / 'start'
            barrier.mkdir()
            final = directory / 'aoi-final'
            final.mkdir()
            # 模拟另外两个网关的最终静止/观察者标记，不启动服务器集群。
            for index in (1, 2):
                (final / f'external{index}.ready').touch()
                (final / f'external{index}.done').touch()
            (barrier / 'START').touch()
            output = directory / 'load.json'
            env = dict(os.environ, AOI_VERIFY_TOTAL='3')
            proc = subprocess.run([CLIENT, '--port', str(server.port), '--scene', '--duration', '1',
                '--rps', '1', '--barrier-dir', str(barrier), '--output', str(output)],
                env=env, capture_output=True, text=True, timeout=6)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            view = json.loads((final / '100000.view.json').read_text())
            self.assertEqual(view['playerId'], 100000)
            self.assertFalse(view['complete'])  # 假服务没有伪造 AOI 快照。
            self.assertTrue((final / '100000.done').exists())
            self.assertEqual(json.loads(output.read_text())['logout_succeeded'], 1)

    def test_generic_requests_keep_multiple_requests_in_flight(self):
        with FakeGateway() as server, tempfile.TemporaryDirectory() as tmp:
            output = pathlib.Path(tmp) / 'load.json'
            proc = subprocess.run([CLIENT, '--port', str(server.port), '--connections', '2',
                '--request', '9', '8', 'body', '--rps', '8', '--duration', '1', '--drain', '1',
                '--output', str(output)], capture_output=True, text=True, timeout=5)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            result = json.loads(output.read_text())
            self.assertEqual(result['business_succeeded'], result['load_attempted'])
            self.assertGreaterEqual(result['business_succeeded'], 12)

    def test_warmup_pending_responses_do_not_pollute_load_accounting(self):
        with FakeGateway('slow') as server, tempfile.TemporaryDirectory() as tmp:
            output = pathlib.Path(tmp) / 'load.json'
            result = subprocess.run([CLIENT, '--port', str(server.port), '--connections', '2',
                '--scene', '--warmup', '1', '--duration', '1', '--drain', '0', '--rps', '2',
                '--output', str(output)], capture_output=True, text=True, timeout=5)
            summary = json.loads(output.read_text())
            self.assertNotEqual(result.returncode, 0)  # 被阻塞的连接没有完成登出。
            self.assertEqual(summary['load_attempted'], summary['completed'] + summary['unresolved'])
            self.assertEqual(summary['unresolved'], 0)  # 慢连接的在途请求属于预热。
            self.assertEqual(summary['business_succeeded'], 2)
            self.assertEqual(summary['in_flight'], 0)

    def run_client(self, mode='normal', connections=32, scene=False, threads=1):
        with FakeGateway(mode) as server, tempfile.TemporaryDirectory() as tmp:
            output = pathlib.Path(tmp) / 'summary.json'
            proc = subprocess.Popen([CLIENT, '--port', str(server.port), '--connections', str(connections),
                '--io-threads', str(threads), '--rps', '2', '--duration', '1', '--drain', '1',
                '--output', str(output)] + (['--scene'] if scene else []),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            peak = 0
            deadline = time.monotonic() + 8
            while proc.poll() is None and time.monotonic() < deadline:
                try:
                    peak = max(peak, len(list(pathlib.Path(f'/proc/{proc.pid}/task').iterdir())))
                except FileNotFoundError:
                    pass
                time.sleep(.01)
            if proc.poll() is None:
                proc.kill()
            stdout, _ = proc.communicate(timeout=2)
            self.assertTrue(output.exists(), stdout)
            result = json.loads(output.read_text())
            self.assertFalse(server.errors, server.errors)
            self.assertEqual(result['load_attempted'], result['completed'] + result['unresolved'])
            self.assertEqual(result['in_flight'], 0)
            return proc.returncode, result, peak, server.identities, server.requests

    def test_one_thread_handles_32_independent_connections(self):
        code, result, peak, identities, requests = self.run_client()
        self.assertEqual(code, 0)
        self.assertEqual(len(identities), 32)
        self.assertEqual(len(requests), 32)
        self.assertGreaterEqual(result['completed'], 32)
        self.assertLessEqual(peak, 3, 'main + one reactor + optional sampler only')

    def test_buffered_frames_over_budget_are_processed_without_new_socket_data(self):
        code, result, *_ = self.run_client(mode='flood', connections=2, scene=True)
        self.assertEqual(code, 0)
        self.assertEqual(result['business_succeeded'], 4)
        self.assertEqual(result['logout_succeeded'], 2)

    def test_scene_connections_and_multiple_io_threads(self):
        code, result, peak, identities, requests = self.run_client(scene=True, threads=2)
        self.assertEqual(code, 0)
        self.assertEqual(result['logout_succeeded'], 32)
        self.assertEqual(len(identities), 32)
        self.assertLessEqual(peak, 4)

    def test_slow_connection_does_not_block_other_connections(self):
        code, result, peak, identities, requests = self.run_client(mode='slow', scene=True)
        self.assertNotEqual(code, 0)
        self.assertGreaterEqual(result['business_succeeded'], 31)
        self.assertGreater(result['unresolved'], 0)
        self.assertLessEqual(peak, 3)

    def test_wrong_response_identity_preserves_accounting(self):
        code, result, *_ = self.run_client(mode='wrong-id', connections=2, scene=True)
        self.assertNotEqual(code, 0)
        self.assertGreater(result['unknown_responses'], 0)

    def test_disconnect_preserves_accounting(self):
        code, result, *_ = self.run_client(mode='close', connections=2, scene=True)
        self.assertNotEqual(code, 0)
        self.assertGreater(result['transport_failed'], 0)

    def test_business_error_preserves_accounting(self):
        code, result, *_ = self.run_client(mode='business-error', connections=2, scene=True)
        self.assertNotEqual(code, 0)
        self.assertGreater(result['business_failed'], 0)


if __name__ == '__main__':
    unittest.main()