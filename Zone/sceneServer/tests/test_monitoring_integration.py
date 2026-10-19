"""检查 SceneServer 组合关系：调度器和窗口采样器不得存在重复所有者。"""
import pathlib
import unittest


class MonitoringCompositionTests(unittest.TestCase):
    def test_server_delegates_periodic_work_and_finalizes_after_business_threads(self):
        root = pathlib.Path(__file__).resolve().parents[1] / 'src-cpp'
        header = (root / 'sceneServer.h').read_text()
        source = (root / 'sceneServer.cc').read_text()
        self.assertIn('std::unique_ptr<SceneMonitoringService> monitoring_', header)
        self.assertNotIn('metricsThread_', header)
        self.assertNotIn('diagnosticThread_', header)
        self.assertNotIn('MetricsSampler metricsSampler_', header)
        self.assertLess(source.index('monitoring_->prepare()'), source.index('netServer_->start()'))
        stop = source[source.index('void SceneServer::stop()'):source.index('void SceneServer::reloadLua')]
        self.assertLess(stop.index('monitoring_->stopPeriodic()'), stop.index('netServer_->stop()'))
        self.assertLess(stop.index('dbThread_->stop()'), stop.index('monitoring_->finish()'))
        self.assertIn('return monitoring_->snapshot()', source)


if __name__ == '__main__':
    unittest.main()