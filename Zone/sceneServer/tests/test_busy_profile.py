import pathlib
import tempfile
import unittest
from unittest.mock import Mock, patch

import busy_profile as profile


class ProfileTests(unittest.TestCase):
    def test_windows_require_start_and_live_scenes(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = profile.Capture(pathlib.Path(tmp))
            capture.tick(None, 50, [100, 200])
            capture.tick(0, 12, [])
            self.assertEqual(capture.records, [])
            capture.close()

    def test_missed_window_is_not_shifted(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = profile.Capture(pathlib.Path(tmp))
            capture.tick(0, 35, [100, 200])
            self.assertEqual(capture.records[0]['status'], 'missed')
            capture.close()

    @patch('busy_profile.subprocess.Popen')
    def test_window_bounded_and_failure_persisted(self, popen):
        process = Mock(pid=999, returncode=0)
        process.poll.return_value = None
        popen.return_value = process
        with tempfile.TemporaryDirectory() as tmp:
            capture = profile.Capture(pathlib.Path(tmp))
            capture.tick(0, 11, [100, 200])
            argv = popen.call_args.args[0]
            self.assertIn('100,200', argv)
            self.assertEqual(argv[-1], '19')
            self.assertTrue(popen.call_args.kwargs['start_new_session'])
            process.poll.return_value = 1
            capture.tick(0, 32, [100, 200])
            self.assertEqual(capture.records[0]['status'], 'failed')
            capture.close()

    @patch('busy_profile.os.killpg')
    @patch('busy_profile.subprocess.Popen')
    def test_close_reaps_sampler_group(self, popen, killpg):
        process = Mock(pid=999, returncode=-2)
        process.poll.return_value = None
        popen.return_value = process
        with tempfile.TemporaryDirectory() as tmp:
            capture = profile.Capture(pathlib.Path(tmp))
            capture.tick(0, 10, [100])
            capture.close()
            killpg.assert_called()
            process.wait.assert_called()
            self.assertEqual(capture.records[0]['status'], 'interrupted')

    @patch('busy_profile.subprocess.Popen', side_effect=OSError('not available'))
    def test_launch_failure_is_diagnostic_not_business_failure(self, popen):
        with tempfile.TemporaryDirectory() as tmp:
            capture = profile.Capture(pathlib.Path(tmp))
            capture.tick(0, 10, [100])
            self.assertEqual(capture.records[0]['status'], 'failed')
            capture.close()

    @patch('busy_profile.subprocess.Popen')
    def test_early_success_exit_is_incomplete(self, popen):
        process = Mock(pid=999, returncode=0)
        process.poll.return_value = None
        popen.return_value = process
        with tempfile.TemporaryDirectory() as tmp:
            capture = profile.Capture(pathlib.Path(tmp))
            capture.tick(0, 10, [100])
            process.poll.return_value = 0
            capture.tick(0, 12, [100])
            self.assertEqual(capture.records[0]['status'], 'incomplete')
            capture.close()


if __name__ == '__main__':
    unittest.main()