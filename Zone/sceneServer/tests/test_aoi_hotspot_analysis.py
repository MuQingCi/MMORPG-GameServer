#!/usr/bin/env python3
import importlib.util
import pathlib
import unittest

MODULE = pathlib.Path(__file__).with_name('report_aoi_hotspot.py')


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), 'AOI normalized analysis missing')
        spec = importlib.util.spec_from_file_location('aoi_report', MODULE)
        self.report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.report)

    def test_counter_reset_rejected(self):
        with self.assertRaises(ValueError):
            self.report.counter_delta(10, 9)
        self.assertEqual(self.report.counter_delta(3, 9), 6)

    def test_normalization(self):
        self.assertIsNone(self.report.per_unit(1, 0))
        self.assertEqual(self.report.per_unit(9, 3), 3)

    def test_histogram_range(self):
        self.assertEqual(self.report.quantile_range([1] + [0]*9, .99), (0, 100))
        self.assertEqual(self.report.quantile_range([0]*9 + [1], .99), (100001, None))
        self.assertIsNone(self.report.quantile_range([0]*10, .99))

    def test_boundaries_no_interpolation(self):
        rows = [dict(monotonicUs=90), dict(monotonicUs=110), dict(monotonicUs=190),
                dict(monotonicUs=210)]
        a, b = self.report.select_window(rows, 100, 200)
        self.assertEqual((a['monotonicUs'], b['monotonicUs']), (110, 190))
        with self.assertRaises(ValueError):
            self.report.select_window(rows, 100, 115)


if __name__ == '__main__':
    unittest.main()