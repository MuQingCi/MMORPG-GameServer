import unittest
from db_maintenance_analysis import summarize


def sample(time,total,history,count,scan_us):
    phases=[dict(count=count,sumUs=count) for _ in range(10)]
    phases[7]=dict(count=count,sumUs=scan_us)
    phases[8]=dict(count=count,sumUs=scan_us)
    classes=[dict(rejected=0,handled=dict(sumUs=count)) for _ in range(6)]
    work=dict(phases=phases,classes=classes,dbTasks=[total,0,total-history,0,history,0],
              dbScanned=[total*count,total*count],netPending=[0]*10)
    return dict(monotonicUs=time,work=[work],depths=[total],netQueue=2,events=[])


class MaintenanceAnalysisTests(unittest.TestCase):
    def test_separates_phase_subsets_and_sampling(self):
        first=sample(1000000,2,1,10,20)
        last=sample(31000000,5,4,30,120)
        value=summarize([[first,last]],1000000,30)
        row=value['windows'][0]
        self.assertEqual(row['dbTasksStart'],[2,0,1,0,1,0])
        self.assertEqual(row['dbTasksEnd'],[5,0,1,0,4,0])
        self.assertEqual(row['maintenanceCalls'],20)
        self.assertEqual(row['twoScanMeanUs'],10)
        self.assertEqual(row['dbScanned'],[130,130])
        self.assertAlmostEqual(row['scanShareOfMeasuredWork'],200/200)
        self.assertEqual(row['phaseUs'][9],20)

    def test_missing_boundary_is_not_silently_extrapolated(self):
        with self.assertRaises(ValueError):
            summarize([[sample(2000000,2,1,10,20)]],1000000,30)

    def test_counter_reset_is_invalid(self):
        with self.assertRaises(ValueError):
            summarize([[sample(1000000,2,1,20,120),sample(31000000,5,4,10,20)]],1000000,30)

    def test_unequal_worker_cardinality_is_invalid(self):
        first=sample(1000000,2,1,10,20)
        last=sample(31000000,5,4,30,120)
        last['work'].append(last['work'][0])
        with self.assertRaises(ValueError):
            summarize([[first,last]],1000000,30)


if __name__=='__main__':
    unittest.main()