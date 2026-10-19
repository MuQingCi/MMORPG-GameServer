import unittest
from analyze_work_diagnostics import summarize


class WorkAnalysisTests(unittest.TestCase):
    def test_rejects_missing_boundaries(self):
        with self.assertRaises(ValueError):
            summarize([],1000,30)

    def test_negative_counter_delta_is_invalid(self):
        def row(t,count):
            duration=dict(count=count,sumUs=count*10,maxUs=10,bins=[count]+[0]*9)
            category=dict(attempted=count,accepted=count,rejected=0,firstRejectedUs=0,handled=duration)
            return dict(monotonicUs=t,depths=[1],work=[dict(classes=[category]*6,phases=[duration]*4)])
        with self.assertRaises(ValueError):
            summarize([row(1000,5),row(1001000,3)],1000,1)

    def test_window_and_first_rejection_offset(self):
        def row(t,count):
            d=dict(count=count,sumUs=count*10,maxUs=10,bins=[count]+[0]*9)
            c=dict(attempted=count*2,accepted=count,rejected=count,firstRejectedUs=1100,handled=d)
            return dict(monotonicUs=t,depths=[count],work=[dict(classes=[c]*6,phases=[d]*4)])
        value=summarize([row(900,1),row(1000900,3)],1000,1)
        self.assertEqual(value['workers'][0]['classes']['business']['attempted'],4)
        self.assertEqual(value['workers'][0]['classes']['business']['firstRejectedOffsetUs'],100)
        self.assertEqual(value['workers'][0]['sampledDepthPeak'],3)


if __name__=='__main__':unittest.main()