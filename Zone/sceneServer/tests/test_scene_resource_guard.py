"""Small subprocess checks for the external resource guard; no server load."""
import os
import pathlib
import subprocess
import tempfile
import time
import unittest

GUARD = pathlib.Path(__file__).with_name('scene_resource_guard.sh')


class GuardTests(unittest.TestCase):
    def run_guard(self, command, **limits):
        with tempfile.TemporaryDirectory() as tmp:
            evidence = pathlib.Path(tmp)
            env = dict(os.environ, GUARD_MIN_AVAILABLE_KB='1')
            env.update(limits)
            unit = f'gs-guard-test-{os.getpid()}-{time.monotonic_ns()}'
            result = subprocess.run(['bash', str(GUARD), unit, tmp, *command],
                                    env=env, capture_output=True, text=True, timeout=15)
            record = (evidence / 'guard.log').read_text() if (evidence / 'guard.log').exists() else ''
            state = subprocess.run(['systemctl', '--user', 'is-active', unit], capture_output=True, text=True)
            self.assertNotEqual(state.stdout.strip(), 'active')
            return result, record

    def test_success_is_not_an_abort(self):
        result, record = self.run_guard(['/bin/sleep', '2'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('ABORT', record)
        self.assertIn('memory.current=', record)

    def test_low_available_memory_aborts_and_cleans_unit(self):
        result, record = self.run_guard(['/bin/sleep', '10'], GUARD_MIN_AVAILABLE_KB='999999999')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ABORT available-memory', record)

    def test_file_growth_aborts(self):
        result, record = self.run_guard(['/bin/sh', '-c', 'dd if=/dev/zero of=large.log bs=4096 count=1 2>/dev/null; sleep 10'],
                                        GUARD_FILE_MAX_BYTES='1024')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ABORT file-limit', record)


if __name__ == '__main__':
    unittest.main()