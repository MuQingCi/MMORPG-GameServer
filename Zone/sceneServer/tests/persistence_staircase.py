#!/usr/bin/env python3
"""Bounded isolated persistence staircase with paired no-DB controls."""
import argparse
import json
import os
import pathlib
import subprocess
import time

from persistence_load_e2e import ZONE, main as persistence_run, validate_options, load_environment
from load_resources import sample_processes


def client_passed(summary, rps, duration):
    return (all(summary[key] == 0 for key in
                ("errors", "business_failed", "timeouts", "unresolved", "decode_errors", "drain_final_unfinished"))
            and summary["logout_succeeded"] == summary["connections"]
            and summary["completed"] == summary["load_attempted"]
            and summary["completed"] >= .9 * summary["connections"] * rps * duration
            and summary["p99_us"] <= 100000)


def run_stages(stages, run_one):
    result = dict(passed=True, runs=[])
    for size in stages:
        for mode in ("persistence", "nodb"):
            row = run_one(size, mode)
            result["runs"].append(row)
            if not row["passed"]:
                result["passed"] = False
    return result


def collect_clients(work, duration, size, rps):
    rows = []
    for g in (1, 2):
        try:
            c = json.loads((work / f"load-gw{g}.json").read_text())
        except (OSError, ValueError):
            continue
        rows.append(dict(gateway=g, targetRps=size*rps, attempted=c["load_attempted"],
            sent=c.get("sent"), completed=c["completed"],
            actualSendRps=round(c.get("sent", c["load_attempted"])/duration, 2),
            successfulRps=round(c["business_succeeded"]/duration, 2),
            p95Us=c["p95_us"], p99Us=c["p99_us"], errors=c["errors"],
            unresolved=c["unresolved"], raw=c,
            loadModel="closed-loop; one outstanding move per connection",
            scheduleDelayMaxUs=c.get("schedule_delay_max_us")))
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("--stages", default="64,96,128,192,256,384,512")
    parser.add_argument("--frame-limit", type=int, default=1000)
    parser.add_argument("--rps", type=int, default=10)
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--drain", type=int, default=30)
    parser.add_argument("--queue-seconds", type=int, default=0)
    parser.add_argument("--recovery", type=int, default=0)
    args = parser.parse_args(argv)
    if not 0 <= args.recovery <= 60:
        parser.error("recovery must be in 0..60")
    stages = [int(s) for s in args.stages.split(',')]
    if not stages or stages != sorted(set(stages)):
        parser.error("stages must be strictly increasing")
    for size in stages:
        validate_options(argparse.Namespace(**{**vars(args), "connections": size}))
    build = args.build.resolve()
    evidence = build / ("persistence-staircase-" + str(time.time_ns()))
    evidence.mkdir(mode=0o700)
    print(f"Staircase evidence: {evidence}", flush=True)

    def run_one(size, mode):
        row = dict(connectionsPerGateway=size, totalConnections=2*size, mode=mode, passed=False,
                   gatewayFrameLimit=args.frame_limit)
        run_dir = evidence / f"{size}-{mode}"
        run_dir.mkdir()
        row["evidence"] = str(run_dir / "persistence" if mode == "persistence" else run_dir)
        artifact = pathlib.Path(row["evidence"])
        work = artifact / "run"
        try:
            if mode == "persistence":
                artifact = persistence_run(str(build), connections=size, rps=args.rps,
                    duration=args.duration, drain=args.drain, queue_seconds=args.queue_seconds,
                    evidence_path=run_dir / "persistence", recovery=args.recovery, frame_limit=args.frame_limit)
                work = artifact / "run"
            else:
                artifact = run_dir
                work = artifact / "run"
                env = load_environment(args.frame_limit)
                env.update(LOAD_CONNECTIONS_PER_GATEWAY=str(size), LOAD_RPS=str(args.rps),
                    LOAD_DURATION=str(args.duration), LOAD_DRAIN=str(args.drain),
                    LOAD_MAX_QUEUE_SECONDS=str(args.queue_seconds), LOAD_EXPLICIT_SCENE="1",
                    LOAD_RECOVERY=str(args.recovery))
                command = ["bash", str(ZONE / "gatewayServer/tests/acceptance_multi_gateway_scene.sh"),
                           str(build / "bin/gatewayServer"), str(build / "bin/sceneServer"), str(ZONE),
                           str(build / "bin/gateway_load_client"), str(work)]
                with (artifact / "runner.log").open("w") as log, (artifact / "resources.jsonl").open("w") as resources:
                    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
                    deadline = time.monotonic() + args.duration + args.drain + args.recovery + 100
                    try:
                        while process.poll() is None:
                            resources.write(json.dumps(sample_processes([process.pid])) + "\n")
                            resources.flush()
                            if time.monotonic() > deadline:
                                raise TimeoutError("no-DB control timed out")
                            time.sleep(1)
                        assert process.returncode == 0, "no-DB runner failed"
                    finally:
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.wait(timeout=40)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()
            row["evidence"] = str(artifact)
            clients = [json.loads((work / f"load-gw{g}.json").read_text()) for g in (1, 2)]
            row["passed"] = all(client_passed(c, args.rps, args.duration) for c in clients)
            if not row["passed"]:
                row["failure"] = "client correctness / achieved rate (<90%) / p99 (>100ms) gate"
        except Exception as error:
            row["failure"] = str(error)
        row["gateways"] = collect_clients(work, args.duration, size, args.rps)
        (run_dir / "result.json").write_text(json.dumps(row, indent=2) + "\n")
        print(json.dumps(row), flush=True)
        return row

    result = run_stages(stages, run_one)
    result["configuration"] = dict(stages=stages, rps=args.rps, duration=args.duration,
                                   drain=args.drain, recovery=args.recovery, queueGuard=False,
                                   gatewayFrameLimit=args.frame_limit,
                                   minAchievedRateRatio=.9, maxP99Us=100000,
                                   placement="dense strip; fixed gateway1->scene10, gateway2->scene20")
    (evidence / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())