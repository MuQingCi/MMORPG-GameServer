import unittest
from analyze_queue_diagnostics import summarize, phase_samples, wait_windows, task_pairs


class QueueSummaryTests(unittest.TestCase):
    def test_windows_empty_samples_and_gap(self):
        rows = [dict(monotonicUs=t, depths=[n], netQueue=0, events=[], waits=[], droppedEvents=0)
                for t, n in ((1000000, 3), (1010000, 2), (1020000, 0), (1100000, 7))]
        report = summarize(rows)
        self.assertEqual(report["emptySamples"], 1)
        self.assertEqual(report["longestSampledNonemptyUs"], 10000)
        self.assertEqual(report["maxSampleGapUs"], 80000)
        self.assertEqual(report["windows"][0]["min"], 0)
        self.assertEqual(report["windows"][1]["max"], 7)
        self.assertAlmostEqual(report["windows"][0]["nonemptyRatio"], 2/3)

    def test_empty_input_rejected(self):
        with self.assertRaises(ValueError):
            summarize([])

    def test_gap_breaks_observed_streak(self):
        rows = [dict(monotonicUs=t, depths=[1], netQueue=0, events=[], waits=[], droppedEvents=0)
                for t in (1000000, 1010000, 1100000, 1110000)]
        report = summarize(rows)
        self.assertEqual(report['longestSampledNonemptyUs'], 10000)
        self.assertEqual(report['samplingGaps'], 1)

    def test_phase_matching_does_not_fill_missing_samples(self):
        rows = [dict(monotonicUs=t, depths=[n], netQueue=0) for t,n in
                ((1000000, 1), (1017000, 0), (2000000, 2), (2017000, 0))]
        result = phase_samples(rows, 1000000, 2, 17000)
        self.assertEqual(result['matched'], 2)
        self.assertEqual(result['nonempty'], 0)
        self.assertEqual(phase_samples(rows,1000000,2,37000)['missing'],2)

    def test_wait_windows_reset_is_invalid(self):
        def row(t,n):
            return dict(monotonicUs=t,waits=[dict(worker=0,category=0,count=n,sumUs=n*10,
                        bins=[n]+[0]*9,missing=0)])
        result=wait_windows([row(1,3),row(1000001,5),row(2000001,1)])
        self.assertEqual(result[0]['count'],2)
        self.assertEqual(result[0]['meanUs'],10)
        self.assertFalse(result[1]['valid'])

    def test_save_pairing_missing_duplicate_and_duration(self):
        def event(kind,begin,end,task=7):
            return dict(kind=kind,beginUs=begin,endUs=end,count=task,worker=0)
        events=[event('save_registered',10,15),event('save_db_accepted',12,14),
                event('save_db_execute',20,40),event('save_completed',50,50)]
        record=task_pairs(events)[0]
        self.assertEqual(record['missing'],[])
        self.assertEqual(record['totalUs'],40)
        self.assertEqual(record['executeUs'],20)
        self.assertEqual(record['returnAndApplyUs'],10)
        self.assertIn('save_completed',task_pairs(events[:-1])[0]['missing'])
        self.assertIn('save_completed',task_pairs(events+[events[-1]])[0]['duplicateStages'])


if __name__ == "__main__":
    unittest.main()