"""可复现的四拓扑运行器契约测试。"""
import unittest
import pathlib
import tempfile
import subprocess
import benchmark_matrix as matrix


class MatrixTests(unittest.TestCase):
    def test_database_bootstrap_preserves_failure_output_and_small_redo(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = pathlib.Path(tmp)
            fake = work / 'bootstrap'
            fake.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\necho bootstrap-diagnostic >&2\nexit 7\n')
            fake.chmod(0o700)
            with self.assertRaises(subprocess.CalledProcessError) as caught:
                matrix.initialize_database(work / 'data', work / 'bootstrap.log', str(fake))
            self.assertEqual(caught.exception.returncode, 7)
            log = (work / 'bootstrap.log').read_text()
            self.assertIn('bootstrap-diagnostic', log)
            self.assertIn('--innodb-log-file-size=16777216', log)

    def test_lifecycle_profile_is_small_and_explicit(self):
        args = matrix.lifecycle_client_args()
        self.assertEqual(args, ['--warmup', '0', '--idle-before', '15', '--hold-after', '20',
                                '--preparation-timeout', '30', '--stagger'])

    def test_approved_six_cases_and_three_gateway_distribution(self):
        selected = matrix.approved_cases()
        self.assertEqual(len(selected), 6)
        self.assertEqual(sum(c['duration'] for c in selected), 270)
        self.assertTrue(all(c['gateways'] == 3 and c['scenes'] == 2 and
                            c['persistence'] and c['rps'] == 20 for c in selected))
        for total, expected in ((128, [43,43,42]), (256, [86,85,85]), (512, [171,171,170])):
            self.assertEqual(matrix.distribute(total, 3), expected)
            players = matrix.player_ids(10000000, total, 3)
            self.assertEqual(len(players), total)
            self.assertEqual(len(set(players)), total)
            self.assertTrue(all(pid < 10009000 for pid in players))

    def test_completion_uses_exact_case_set(self):
        selected = matrix.approved_cases()
        actual = [matrix.identity(c) for c in selected]
        self.assertTrue(matrix.exact_case_set(selected, actual))
        self.assertFalse(matrix.exact_case_set(selected, actual[:-1]))
        self.assertFalse(matrix.exact_case_set(selected, actual + [actual[0]]))
        self.assertFalse(matrix.exact_case_set(selected, actual + ['extra']))

    def test_matrix_and_frame_boundary(self):
        cases = matrix.cases()
        self.assertEqual(len(cases), 480)
        self.assertEqual(sum(c['duration'] for c in cases), 26400)
        self.assertEqual(matrix.frame_limit(1000, 10), 10000)
        self.assertEqual(matrix.frame_limit(1024, 10), 100000)

    def test_globally_merged_nearest_rank(self):
        self.assertEqual(matrix.percentiles([1, 2, 3, 100, 200, 300, 400, 500, 600, 700]),
                         dict(p50=200, p90=600, p99=700))
        self.assertEqual(matrix.percentiles([]), dict(p50=None, p90=None, p99=None))

    def test_yaml_replaces_instead_of_duplicates(self):
        text = 'SceneServer:\n    net:\n        maxFramesPerSec: 1000 # original\n'
        changed = matrix.replace_scalar(text, 'maxFramesPerSec', 100000)
        self.assertEqual(changed.count('maxFramesPerSec:'), 1)
        self.assertIn('maxFramesPerSec: 100000', changed)
        with self.assertRaises(ValueError):
            matrix.replace_scalar(text + text, 'maxFramesPerSec', 4)


if __name__ == '__main__':
    unittest.main()