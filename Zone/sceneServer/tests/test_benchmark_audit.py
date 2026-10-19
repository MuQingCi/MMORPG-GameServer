"""独立的原始证据审计器必须拒绝损坏的缓存分位数。"""
import gzip
import importlib
import importlib.util
import json
import pathlib
import tempfile
import unittest


class AuditTests(unittest.TestCase):
    def test_detects_corrupt_cached_percentile_without_modifying_evidence(self):
        self.assertIsNotNone(importlib.util.find_spec('benchmark_audit'),
                             'independent raw-evidence auditor is missing')
        audit = importlib.import_module('benchmark_audit')
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            case = dict(gateways=1, scenes=1, connections=1, rps=10,
                        persistence=False, duration=10)
            (work / 'result.json').write_text(json.dumps(dict(case=case, passed=True)))
            (work / 'load-gw1.json').write_text(json.dumps(dict(connections=1,
                load_attempted=3, completed=3, unresolved=0, business_succeeded=3)))
            with gzip.open(work / 'load-gw1.json.latency-us.txt.gz', 'wt') as output:
                output.write('10\n20\n30\n')
            cached = dict(p50Us=20, p90Us=30, p99Us=30, latencySamples=3)
            cache = work / 'case-summary.json'
            cache.write_text(json.dumps(cached))
            for name in ('gw1.yaml', 'scene1.yaml'):
                (work / name).write_text('    net:\n        maxFramesPerSec: 10000\n')
            self.assertEqual(audit.audit_case(work)['issues'], [])
            cached['p90Us'] = 999
            cache.write_text(json.dumps(cached))
            before = cache.read_bytes()
            self.assertIn('p90Us differs from raw samples', audit.audit_case(work)['issues'])
            self.assertEqual(before, cache.read_bytes())


if __name__ == '__main__':
    unittest.main()