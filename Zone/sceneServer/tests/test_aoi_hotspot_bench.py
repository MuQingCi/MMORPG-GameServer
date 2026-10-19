#!/usr/bin/env python3
import json
import pathlib
import subprocess
import sys
import unittest


class BenchTests(unittest.TestCase):
    def test_reference_workloads(self):
        binary = pathlib.Path(sys.argv[1])
        self.assertTrue(binary.is_file(), 'deterministic AOI reference benchmark missing')
        for layout in ('dense', 'sparse'):
            for moving in (0, 1):
                run = subprocess.run([str(binary), '16', layout, str(moving), '3', '1'],
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                row = json.loads(run.stdout)
                self.assertEqual(row['referenceMismatches'], 0)
                self.assertEqual(row['entities'], 16)
                self.assertEqual(row['ticks'], 3)
                self.assertEqual(len(row['aoi']['stages']), 7)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])