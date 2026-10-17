"""Opt-in real-process monitoring checks; never connects to a database.

Set SCENE_MONITORING_BINARY to the absolute sceneServer executable path.
"""
import json
import os
import pathlib
import re
import signal
import subprocess
import tempfile
import time
import unittest


@unittest.skipUnless(os.environ.get('SCENE_MONITORING_BINARY'), 'explicit process binary required')
class MonitoringProcessTests(unittest.TestCase):
    def launch(self, directory, diagnostics, invalid_path=False):
        source = pathlib.Path(__file__).resolve().parents[1]
        zone = (source.parent / 'config/zoneConfig.yaml').read_text()
        zone = zone.replace('enable: true', 'enable: false')
        zone = re.sub(r'(?m)^        dir:.*$', f'        dir: {directory}/logs', zone)
        zone = re.sub(r'(?m)^        - 1=127\.0\.0\.1:\d+$',
                      '        - 1=127.0.0.1:1', zone)
        scene = (source / 'config/sceneConfig.yaml').read_text()
        scene = re.sub(r'(?m)^    luaDir:.*$', f'    luaDir: {source}/lua_script', scene)
        scene += f'\n    mapPath: {source}/config/map.yaml\n'
        metrics = directory / 'metrics.json'
        scene += f'    metricsPath: {directory / "missing" / "metrics.json" if invalid_path else metrics}\n'
        (directory / 'zone.yaml').write_text(zone)
        (directory / 'scene.yaml').write_text(scene)
        env = os.environ.copy()
        env.pop('SCENE_QUEUE_DIAGNOSTICS', None)
        if diagnostics:
            env['SCENE_QUEUE_DIAGNOSTICS'] = '1'
        output = (directory / 'stdout.log').open('w')
        process = subprocess.Popen([os.environ['SCENE_MONITORING_BINARY'],
                                    str(directory / 'zone.yaml'), str(directory / 'scene.yaml')],
                                   cwd=directory, env=env, stdout=output, stderr=subprocess.STDOUT)
        self.addCleanup(output.close)
        self.addCleanup(self.stop_process, process)
        return process, metrics

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

    def test_diagnostic_mode_adds_no_thread_and_exports_final_snapshot(self):
        counts = []
        for diagnostics in (False, True):
            with tempfile.TemporaryDirectory(prefix='monitoring-process-') as tmp:
                directory = pathlib.Path(tmp)
                process, metrics = self.launch(directory, diagnostics)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    self.assertIsNone(process.poll(), (directory / 'stdout.log').read_text())
                    try:
                        if json.loads(metrics.read_text())['sampleSequence'] >= 2:
                            break
                    except (OSError, ValueError, KeyError):
                        pass
                    time.sleep(.02)
                else:
                    self.fail('no second metrics sample')
                counts.append(len(list(pathlib.Path(f'/proc/{process.pid}/task').iterdir())))
                sequence = json.loads(metrics.read_text())['sampleSequence']
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=15), 0)
                final = json.loads(metrics.read_text())
                self.assertGreater(final['sampleSequence'], sequence)
                self.assertFalse(final['monitor_metricsWriteFailed'])
                queues = pathlib.Path(str(metrics) + '.queues.jsonl')
                self.assertEqual(queues.exists(), diagnostics)
                if diagnostics:
                    rows = [json.loads(line) for line in queues.read_text().splitlines()]
                    self.assertTrue(rows[-1]['fullSnapshot'])
                    self.assertGreater(len(rows), 20)
                    self.assertTrue(all(not r['writeFailed'] for r in rows))
                    self.assertEqual(rows[-1]['droppedEvents'], 0)
        self.assertEqual(counts[0], counts[1])

    def test_invalid_diagnostic_path_exits_without_business_start(self):
        with tempfile.TemporaryDirectory(prefix='monitoring-process-invalid-') as tmp:
            directory = pathlib.Path(tmp)
            process, metrics = self.launch(directory, True, invalid_path=True)
            self.assertEqual(process.wait(timeout=10), 1)
            self.assertFalse(metrics.exists())
            logs = ''.join(p.read_text() for p in (directory / 'logs').rglob('*.log'))
            self.assertIn('scene server start failed', logs)


if __name__ == '__main__':
    unittest.main()