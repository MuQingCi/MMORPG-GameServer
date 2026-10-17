#!/usr/bin/env python3
import unittest
import importlib.util
import argparse

import persistence_e2e as harness


class PersistenceHarnessTests(unittest.TestCase):
    def test_frame_limit_is_explicit_validated_and_shared_by_both_modes(self):
        from persistence_load_e2e import parse_args, load_environment
        args = parse_args(['/tmp/build', '--frame-limit', '100000'])
        self.assertEqual(args.frame_limit, 100000)
        self.assertEqual(parse_args(['/tmp/build']).frame_limit, 1000)
        for invalid in ('0', '-1', '4294967296'):
            with self.assertRaises(ValueError):
                parse_args(['/tmp/build', '--frame-limit', invalid])
        from unittest.mock import patch
        with patch.dict('os.environ', {'LOAD_FRAME_LIMIT': '7', 'LOAD_RPS': '999'}):
            env = load_environment(100000)
        self.assertEqual(env['LOAD_FRAME_LIMIT'], '100000')
        self.assertNotIn('LOAD_RPS', env)

    def load_runner(self):
        spec = importlib.util.spec_from_file_location("load_harness", harness.ZONE / "sceneServer/tests/persistence_load_e2e.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_load_options_are_bounded_and_keep_old_defaults(self):
        module = self.load_runner()
        args = module.parse_args(["/tmp/build"])
        self.assertEqual((args.connections, args.rps, args.duration, args.drain), (8, 10, 10, 5))
        args = module.parse_args(["/tmp/build", "--connections", "64", "--duration", "30", "--drain", "10"])
        self.assertEqual(args.connections, 64)
        for size in (64, 96, 128, 512):
            self.assertEqual(module.parse_args(["/tmp/build", "--connections", str(size)]).connections, size)
        for flag, value in (("--connections", "513"), ("--rps", "0"), ("--duration", "3"), ("--drain", "0")):
            with self.assertRaises(ValueError):
                module.validate_options(argparse.Namespace(**{**vars(args), flag[2:]: int(value)}))
        module.parse_args(["/tmp/build", "--connections", "512", "--rps", "20", "--duration", "300"])

    def test_resource_sample_has_self_and_cpu_counters(self):
        import os
        from load_resources import sample_processes
        sample = sample_processes([os.getpid()])
        row = next(p for p in sample["processes"] if p["pid"] == os.getpid())
        self.assertGreater(sample["clockTicks"], 0)
        self.assertGreater(row["rssBytes"], 0)
        self.assertGreater(row["startTicks"], 0)
        self.assertGreaterEqual(row["cpuTicks"], 0)

    def test_maximum_seed_stays_in_known_obstacle_free_strip(self):
        import re
        module = self.load_runner()
        text = (harness.ZONE / "sceneServer/config/map.yaml").read_text()
        width, height = map(int, re.search(r'map_size:\s*(\d+),(\d+)', text).groups())
        blocks = [tuple(map(int, match)) for match in
                  re.findall(r'- block_\d+:\s*(\d+),(\d+),(\d+),(\d+)', text)]
        for size in (64, 96, 128, 512):
            rows = module.seed_manifest(size)
            self.assertEqual(len(rows), size * 2)
            self.assertEqual(len({r["playerId"] for r in rows}), size * 2)
            for row in rows:
                self.assertEqual(row["sceneId"], row["gateway"] * 10)
                self.assertEqual(row["saveVersion"], 7)
                self.assertEqual(row["y"], 6)
                self.assertTrue(0 < row["y"] < height)
                for x in range(row["x"], row["x"] + 10 * 30 + 1):
                    self.assertTrue(0 < x < width)
                    self.assertFalse(any(abs(x-cx) <= bw/2 and abs(row["y"]-cy) <= bh/2
                                         for cx, cy, bw, bh in blocks))
        for size in (0, 513):
            with self.assertRaises(ValueError):
                module.seed_manifest(size)

    def test_expanded_staircase_continues_after_failure(self):
        import persistence_staircase as stairs
        called = []
        def run_one(size, mode):
            called.append((size, mode))
            return {"passed": size < 96}
        result = stairs.run_stages([64, 96, 128], run_one)
        self.assertFalse(result["passed"])
        self.assertEqual(called, [(size, mode) for size in (64, 96, 128) for mode in ("persistence", "nodb")])

    def test_staircase_runs_failed_mode_control_and_next_stage(self):
        import persistence_staircase as stairs
        called = []
        def run_one(size, mode):
            called.append((size, mode))
            return {"passed": size != 16, "connectionsPerGateway": size, "mode": mode}
        result = stairs.run_stages([8, 16, 32], run_one)
        self.assertFalse(result["passed"])
        self.assertEqual(called, [(size, mode) for size in (8, 16, 32) for mode in ("persistence", "nodb")])

    def test_staircase_rejects_low_achieved_rate_and_errors(self):
        import persistence_staircase as stairs
        summary = dict(errors=0, business_failed=0, timeouts=0, unresolved=0,
                       decode_errors=0, logout_succeeded=8, connections=8,
                       completed=2400, load_attempted=2400, drain_final_unfinished=0, p99_us=2000)
        self.assertTrue(stairs.client_passed(summary, 10, 30))
        self.assertFalse(stairs.client_passed({**summary, "completed": 10}, 10, 30))
        self.assertFalse(stairs.client_passed({**summary, "errors": 1}, 10, 30))

    def test_failed_runner_keeps_partial_client_measurements(self):
        import tempfile, pathlib, json
        import persistence_staircase as stairs
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / 'load-gw1.json').write_text(json.dumps(dict(load_attempted=50,
                business_succeeded=40, completed=45, errors=5, p95_us=100, p99_us=200,
                unresolved=5)))
            rows = stairs.collect_clients(work, 10, 8, 10)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['actualSendRps'], 5)
            self.assertEqual(rows[0]['successfulRps'], 4)

    def test_zone_peer_matches_gateway_identity_and_isolated_database(self):
        template = (harness.ZONE / "config/zoneConfig.yaml").read_text()
        self.assertTrue(callable(getattr(harness, "make_zone_config", None)),
                        "isolation config must share gateway identity")
        result = harness.make_zone_config(template, 41001, 41002, 41003,
                                          "test_password", "test_database", 3)
        self.assertIn("3=127.0.0.1:41003", result)
        self.assertNotIn("1=127.0.0.1:13145", result)
        self.assertIn("port: 41001", result)
        self.assertIn("port: 41002", result)
        self.assertIn("dbName: test_database", result)
        self.assertIn("user: persistence_test", result)

    def test_multi_runner_has_explicit_isolated_mode_and_checked_shutdown(self):
        runner = (harness.ZONE / "gatewayServer/tests/acceptance_multi_gateway_scene.sh").read_text()
        self.assertIn("LOAD_PERSISTENCE_ZONE_CONFIG", runner)
        self.assertIn("LOAD_PERSISTENCE_CONTEXT", runner)
        self.assertIn('wait "$scene_pid"', runner)

    def test_seed_manifest_matches_gateway_routing_and_valid_snapshot(self):
        path = harness.ZONE / "sceneServer/tests/persistence_load_e2e.py"
        self.assertTrue(path.is_file(), "isolated multi-gateway runner must exist")
        spec = importlib.util.spec_from_file_location("load_harness", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        rows = module.seed_manifest(8)
        self.assertEqual(len(rows), 16)
        self.assertEqual(len({r["playerId"] for r in rows}), 16)
        for row in rows:
            self.assertEqual(row["sceneId"], row["gateway"] * 10)
            self.assertEqual(row["saveVersion"], 7)
            self.assertGreater(row["x"], 0)
            self.assertGreater(row["y"], 0)
        with self.assertRaises(ValueError):
            module.seed_manifest(0)


if __name__ == "__main__":
    unittest.main()