#!/usr/bin/env python3
"""生成的 Gateway/区服身份及端点配置的回归检查。"""
import pathlib
import re
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNER = ROOT / "gatewayServer/tests/run_scene_load.sh"
MULTI_RUNNER = ROOT / "gatewayServer/tests/acceptance_multi_gateway_scene.sh"
LOAD_CLIENT = ROOT / "gatewayServer/tests/load_client.cc"


def extract_config_identity(source: str):
    gateway_id = re.search(r"^\s*gatewayId:\s*(\d+)\s*$", source, re.M)
    peer_id = re.search(r"^\s*-\s*(\d+)=127\.0\.0\.1:(\d+)\s*$", source, re.M)
    if not gateway_id or not peer_id:
        raise AssertionError("generated config missing gateway or peer identity")
    return int(gateway_id.group(1)), int(peer_id.group(1)), int(peer_id.group(2))


class LoadHarnessConfigTests(unittest.TestCase):
    def test_runner_generates_the_same_gateway_identity_on_both_sides(self):
        runner = RUNNER.read_text()
        gateway_match = re.search(r"^\s+gatewayId:\s*(\d+|\$\{?[A-Z_]+\}?)\s*$", runner, re.M)
        self.assertIsNotNone(gateway_match, "runner must define a gateway identity")
        self.assertIn("GATEWAY_ID", runner, "identity must be a shared configurable value")
        self.assertIn("GATEWAY_ID", runner[runner.index('"$ROOT/config/zoneConfig.yaml"'):],
                      "zone peer identity must use the same value")

        zone_template = (ROOT / "config/zoneConfig.yaml").read_text()
        peer_match = re.search(r"^\s*-\s*(\d+)=127\.0\.0\.1:13145\s*$", zone_template, re.M)
        self.assertIsNotNone(peer_match, "source zone config must contain a gateway peer")
        if gateway_match.group(1).isdigit():
            self.assertEqual(int(gateway_match.group(1)), int(peer_match.group(1)))

    def test_generated_config_identity_matches_and_uses_private_port(self):
        with tempfile.TemporaryDirectory() as tmp:
            gateway = pathlib.Path(tmp) / "gateway.yaml"
            zone = pathlib.Path(tmp) / "zone.yaml"
            gateway.write_text("gateway:\n    gatewayId: 7\n    privateConfig:\n        listenPort: 25001\n")
            zone.write_text("ZoneServer:\n    gateways:\n        - 7=127.0.0.1:25001\n")
            gateway_id, peer_id, peer_port = extract_config_identity(
                gateway.read_text() + zone.read_text())
            self.assertEqual(gateway_id, peer_id)
            self.assertEqual(peer_port, 25001)

    def test_multi_runner_collects_gateway_and_scene_metrics_for_both_gateways(self):
        runner = MULTI_RUNNER.read_text()
        self.assertIn("metricsPath: $WORK/gw1.metrics.json", runner)
        self.assertIn("metricsPath: $WORK/gw2.metrics.json", runner)
        for gateway in (1, 2):
            self.assertIn(f'--gateway-metrics "$WORK/gw{gateway}.metrics.json"', runner)
            self.assertIn('--scene-metrics "$WORK/scene10.metrics.json"', runner)
            self.assertIn('--scene-metrics-extra "$WORK/scene20.metrics.json"', runner)
            self.assertIn(f'--metrics-output "$WORK/metrics-gw{gateway}.jsonl"', runner)

    def test_multi_runner_passes_bounded_drain_to_both_clients(self):
        runner = MULTI_RUNNER.read_text()
        self.assertIn('DRAIN="${LOAD_DRAIN:-0}"', runner)
        self.assertEqual(len(re.findall(r'^"\$LOAD_BIN" .* --drain "\$DRAIN" .*& L[12]=\$!', runner, re.M)), 2)
        self.assertIn("LOAD_DRAIN", runner)

    def test_multi_runner_starts_exactly_two_load_client_processes(self):
        runner = MULTI_RUNNER.read_text()
        launches = re.findall(r'^"\$LOAD_BIN" .*& L[12]=\$!', runner, re.M)
        self.assertEqual(len(launches), 2, "one load client process per Gateway")

    def test_multi_runner_accepts_explicit_temporary_frame_limit(self):
        runner = MULTI_RUNNER.read_text()
        self.assertIn('FRAME_LIMIT="${LOAD_FRAME_LIMIT:-1000}"', runner)
        self.assertIn('maxFramesPerSec: $FRAME_LIMIT', runner)

    def test_sampler_emits_final_duration_boundary_sample(self):
        source = LOAD_CLIENT.read_text()
        self.assertIn("elapsedMs", source)
        self.assertIn("sampleIndex <= options.durationSec", source)
        self.assertIn("finalSample", source)
        self.assertIn("if (sampleIndex >= options.durationSec + options.drainSec) break;", source)
        self.assertIn("!options.metricsOutput.empty())", source)

    def test_load_client_waits_for_start_before_beginning_metrics_clock(self):
        source = LOAD_CLIENT.read_text()
        self.assertIn("g_startEpochMs", source)
        self.assertIn("const auto loadStart = Clock::time_point", source)
        runner = MULTI_RUNNER.read_text()
        self.assertNotIn('then touch "$WORK/start-barrier/START"', runner)

    def test_load_client_joins_workers_and_sampler_exactly_once(self):
        source = LOAD_CLIENT.read_text()
        self.assertEqual(source.count("worker.join();"), 1)
        self.assertEqual(source.count("metricsThread = std::thread(SampleMetrics"), 1)
        self.assertEqual(source.count("metricsThread.join();"), 1)
        self.assertIn("while (g_startEpochMs.load() <= 0 && !g_stopRequested)", source)
        self.assertIn("while (Clock::now() < nextSample && !g_stopRequested)", source)
        self.assertIn("!options.additionalSceneMetrics.empty() || !options.metricsOutput.empty())", source)

    def test_load_client_supports_bounded_drain_and_pending_metrics(self):
        source = LOAD_CLIENT.read_text()
        self.assertIn("--drain", source)
        self.assertIn("drainSec", source)
        self.assertIn("in_flight", source)
        self.assertIn("drain_unfinished", source)
        self.assertIn("cutoff_unfinished", source)
        self.assertIn("recordUnresolved", source)
        self.assertIn("SIGTERM", source)
        self.assertIn("business_errors", source)
        self.assertTrue((ROOT / "gatewayServer/tests/test_load_client_behavior.py").exists())
        self.assertNotIn("streak>=limit", MULTI_RUNNER.read_text())
        self.assertNotIn("EARLY_STOP", MULTI_RUNNER.read_text())
        self.assertIn("LOAD_RECOVERY", MULTI_RUNNER.read_text())
        self.assertIn("while (true)", source)
        self.assertIn("drainDeadline", source)
        self.assertIn("g_stopRequested", source)
        self.assertIn('\\"phase\\"', source)

    def test_runner_does_not_print_unconditional_acceptance_success(self):
        runner = MULTI_RUNNER.read_text()
        self.assertNotIn('\necho "PASS: 2 gateways', runner)
        self.assertIn('if (( client_status == 0 && acceptance_status == 0 )); then', runner)


if __name__ == "__main__":
    unittest.main()