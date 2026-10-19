import json
import os
import pathlib
import subprocess
import unittest


class ScanBenchTests(unittest.TestCase):
    def test_modes_report_cost_and_history_visits(self):
        binary = pathlib.Path(os.environ['DB_SCAN_BENCH'])
        outputs = {}
        for mode in ('legacy', 'indexed'):
            result = subprocess.run([str(binary), '--mode', mode], capture_output=True,
                                    text=True, check=True, timeout=60)
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(len(rows), 18)
            for row in rows:
                self.assertEqual(row.get('mode'), mode)
                self.assertEqual(row['callbacks'], 0)
                self.assertGreater(row['cpuNs'], 0)
                self.assertEqual(row['historyVisits'], 0 if mode == 'indexed' else row['history']*2)
                self.assertEqual(row['activeVisits'], 64)
            outputs[mode] = rows
        self.assertEqual({r['history'] for r in outputs['legacy']},
                         {r['history'] for r in outputs['indexed']})


if __name__ == '__main__':
    unittest.main()