import unittest
import tempfile
import pathlib
import json
from persistence_phase_diagnostics import run_batch, analyze_run


class BatchTests(unittest.TestCase):
    def test_first_failure_stops_batch(self):
        calls=[]
        def run(index):
            calls.append(index)
            return dict(passed=index==1)
        report=run_batch(3,run)
        self.assertFalse(report['passed'])
        self.assertEqual(calls,[1,2])
        self.assertEqual(report['stoppedAt'],2)

    def test_exception_is_failure_and_preserves_previous_runs(self):
        def run(index):
            raise RuntimeError('broken artifact')
        report=run_batch(3,run)
        self.assertEqual(len(report['runs']),1)
        self.assertIn('broken artifact',report['runs'][0]['failure'])

    def test_invalid_repeat_count(self):
        for count in (0,4):
            with self.assertRaises(ValueError):
                run_batch(count,lambda _:dict(passed=True))

    def test_partial_failure_preserves_both_scene_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            path=pathlib.Path(directory);work=path/'run';work.mkdir()
            (work/'metrics-gw1.jsonl').write_text(json.dumps(dict(readMonotonicUs=1000000))+'\n')
            for scene in (10,20):
                row=dict(monotonicUs=1000000,depths=[1],netQueue=0,events=[],waits=[],
                         droppedEvents=0,fullSnapshot=True)
                (work/f'scene{scene}.metrics.json.queues.jsonl').write_text(json.dumps(row)+'\n')
            reports=analyze_run(path,require_complete=False)
            self.assertEqual(set(reports),{'10','20'})
            self.assertTrue((work/'scene10.metrics.json.queues.summary.json').exists())
            self.assertFalse(reports['10']['valid'])


if __name__=='__main__':
    unittest.main()