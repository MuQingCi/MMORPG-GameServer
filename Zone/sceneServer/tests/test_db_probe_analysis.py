import unittest
from db_probe_analysis import probe_delta


class ProbeAnalysisTests(unittest.TestCase):
    def test_cpu_callback_exclusion_and_state_fraction(self):
        before=[[0]*12 for _ in range(2)]
        after=[[2,0,2,0,98,0,1,4000,3000,1000,500,0] for _ in range(2)]
        value=probe_delta(before,after)[0]
        self.assertEqual(value['samples'],2)
        self.assertEqual(value['historyVisitFraction'],.98)
        self.assertEqual(value['cpuUsPerCall'],1.5)
        self.assertEqual(value['nonCallbackCpuUsPerCall'],1.25)
        self.assertEqual(value['cpuClockFailures'],0)

    def test_empty_samples_have_no_mean(self):
        zero=[[0]*12 for _ in range(2)]
        self.assertIsNone(probe_delta(zero,zero)[0]['cpuUsPerCall'])

    def test_reset_rejected(self):
        with self.assertRaises(ValueError):
            probe_delta([[1]*12 for _ in range(2)],[[0]*12 for _ in range(2)])


if __name__=='__main__':
    unittest.main()