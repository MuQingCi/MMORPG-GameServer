import pathlib
import tempfile
import unittest

from report_logic_consumption import expected_names, report
from logic_consumption_experiment import formal_schedule


class LogicReportTests(unittest.TestCase):
    def test_approved_exact_18_cases(self):
        selected=expected_names()
        self.assertEqual(len(selected),18)
        self.assertIn('B-c512-d60-rep3',selected)
        self.assertNotIn('A-c128-d30-rep1',selected)
        schedule=formal_schedule()
        self.assertEqual(len(schedule),18)
        self.assertEqual(schedule[:4],[('A',256,30,1),('B',256,30,1),
                                       ('B',256,30,2),('A',256,30,2)])
        self.assertEqual({f'{v}-c{c}-d{d}-rep{rep}' for v,c,d,rep in schedule},selected)

    def test_incomplete_formal_set_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                report(pathlib.Path(tmp),True)

    def test_unexpected_batch_without_results_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            (pathlib.Path(tmp)/'A-unexpected').mkdir()
            with self.assertRaises(ValueError):
                report(pathlib.Path(tmp),True)


if __name__=='__main__':unittest.main()