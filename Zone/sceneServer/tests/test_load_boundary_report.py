import unittest

from load_boundary_report import analyze_samples, resource_peaks


class LoadBoundaryReportTests(unittest.TestCase):
    def test_recovery_needs_all_workers_routes_and_inflight_zero(self):
        rows = [dict(elapsedMs=t, readMonotonicUs=t*1000, metricsValid=True,
                     client_sent=n, client_business_succeeded=n,
                     client_in_flight=pending, gateway_pendingRoutes=0,
                     scene0_worker0_queue=depth, scene1_worker0_queue=0)
                for t, n, pending, depth in ((0, 0, 0, 0), (1000, 9, 1, 7),
                                             (2000, 10, 1, 0), (3000, 10, 0, 0))]
        for row in rows:
            row['gateway_backendReady'] = 2
            for scene in (0, 1):
                row[f'scene{scene}_netQueue'] = 0
                for worker in range(4):
                    row.setdefault(f'scene{scene}_worker{worker}_queue', 0)
        report = analyze_samples(rows, 1)
        self.assertEqual(report['sampledRecoveryMs'], 2000)
        self.assertEqual(report['peakWorkerDepths']['scene0_worker0_queue'], 7)
        self.assertEqual(report['loadWindows'][0]['sendRps'], 9)

    def test_partial_worker_snapshot_cannot_prove_recovery(self):
        report = analyze_samples([dict(elapsedMs=2000, metricsValid=True,
            client_in_flight=0, gateway_pendingRoutes=0, scene0_worker0_queue=0)], 1)
        self.assertIsNone(report['sampledRecoveryMs'])

    def test_empty_queues_with_disconnected_backends_are_not_recovery(self):
        row = dict(elapsedMs=2000, metricsValid=True, client_in_flight=0,
                   gateway_pendingRoutes=0, gateway_backendReady=0)
        for s in (0, 1):
            row[f'scene{s}_netQueue'] = 0
            for w in range(4):
                row[f'scene{s}_worker{w}_queue'] = 0
        self.assertIsNone(analyze_samples([row], 1)['sampledRecoveryMs'])

    def test_nominal_sample_time_is_not_used_for_window_rate(self):
        report = analyze_samples([
            dict(elapsedMs=0, readMonotonicUs=1000000, client_sent=0,
                 client_business_succeeded=0),
            dict(elapsedMs=1000, readMonotonicUs=3000000, client_sent=10,
                 client_business_succeeded=8)], 10)
        self.assertEqual(report['loadWindows'][0]['sendRps'], 5)
        self.assertIsNone(report['sampledRecoveryMs'])

    def test_resource_delta_matches_process_identity(self):
        samples = [dict(monotonicMs=t, clockTicks=100,
                        processes=[dict(pid=1, startTicks=2, name='client',
                                        cpuTicks=cpu, rssBytes=100)])
                   for t, cpu in ((0, 10), (1000, 60))]
        peaks = resource_peaks(samples)
        self.assertEqual(peaks['client']['peakCpuPercent'], 50)
        self.assertEqual(peaks['client']['peakRssBytes'], 100)