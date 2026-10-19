#!/usr/bin/env python3
"""显式、隔离的 2 Gateway × 2 Scene 持久化验收（仅使用标准库）。"""
import json
import argparse
import os
import pathlib
import pwd
import re
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

from persistence_e2e import ROOT, ZONE, fields, make_zone_config, port, until, wire
from load_resources import sample_processes

MAX_CONNECTIONS_PER_GATEWAY = 512


def validate_options(args):
    for name, low, high in (("connections", 1, MAX_CONNECTIONS_PER_GATEWAY), ("rps", 1, 20), ("duration", 10, 300),
                           ("drain", 1, 60), ("queue_seconds", 0, 30)):
        if not low <= getattr(args, name) <= high:
            raise ValueError(f"{name} must be in {low}..{high}")
    frame_limit = getattr(args, "frame_limit", 1000)
    if not 1 <= frame_limit <= 4294967295:
        raise ValueError("frame_limit must be in 1..4294967295 (zero does not disable limiting)")
    return args


def load_environment(frame_limit):
    validate_options(argparse.Namespace(connections=1, rps=1, duration=10,
                                        drain=1, queue_seconds=0, frame_limit=frame_limit))
    env = {k: v for k, v in os.environ.items() if not k.startswith("LOAD_")}
    env["LOAD_FRAME_LIMIT"] = str(frame_limit)
    return env


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build")
    parser.add_argument("--frame-limit", type=int, default=1000)
    parser.add_argument("--negative-save", action="store_true")
    parser.add_argument("--queue-diagnostics", action="store_true")
    for name, default in (("connections", 8), ("rps", 10), ("duration", 10), ("drain", 5), ("queue-seconds", 3)):
        parser.add_argument("--" + name, type=int, default=default)
    return validate_options(parser.parse_args(argv))


def seed_manifest(connections):
    if not 1 <= connections <= MAX_CONNECTIONS_PER_GATEWAY:
        raise ValueError(f"correctness acceptance requires 1..{MAX_CONNECTIONS_PER_GATEWAY} connections per gateway")
    return [dict(playerId=base+i, sceneId=gateway*10,
                 x=5+i, y=6, saveVersion=7, gateway=gateway)
            for gateway, base in ((1, 1100000), (2, 2100000))
            for i in range(connections)]


def sql(context, statement):
    result = subprocess.run([
        "mariadb", "--no-defaults", "--protocol=socket",
        "--socket=" + context["socket"], "--user=root", "--batch", "--skip-column-names"],
        input=statement, text=True, capture_output=True, timeout=10)
    if result.returncode:
        raise AssertionError("isolated SQL failed: " + result.stderr.replace(context.get("password", ""), "<redacted>"))
    return result.stdout.strip()


def request(sock, method, rid, body=b""):
    sock.sendall(wire.client_frame(wire.KIND_REQUEST, 3, method, rid, body))
    for _ in range(128):
        response = wire.read_frame(sock)
        assert response, "connection closed before response"
        if response[0] == wire.KIND_PUSH:
            continue
        if response[3] == rid:
            assert response[:4] == (wire.KIND_RESPONSE, 3, method, rid), response[:4]
            return response[4]
    raise AssertionError("response missing")


def connect_player(public_port, player):
    sock = socket.create_connection(("127.0.0.1", public_port), timeout=5)
    sock.settimeout(5)
    try:
        sock.sendall(wire.client_frame(wire.KIND_CONTROL, 1, 1, 1,
                                      struct.pack("<QQ", player, 998877)))
        assert wire.read_frame(sock)[:4] == (wire.KIND_CONTROL, 1, 2, 1)
        return sock
    except BaseException:
        sock.close()
        raise


def rejected_enter(sock):
    body = request(sock, 4, 2)
    # RetTip(code=1010, text=...) 并非 EnterSceneAck 的纯整数字段布局。
    assert body.startswith(b"\x08\xf2\x07\x12"), "expected load-failed RetTip 1010"


def verify(context_path, work):
    context = json.loads(pathlib.Path(context_path).read_text())
    work = pathlib.Path(work)
    database = context["database"]
    manifest = context["players"]
    expected = {}
    for gateway in (1, 2):
        summary = json.loads((work / f"load-gw{gateway}.json").read_text())
        rows = [json.loads(line) for line in (work / f"load-gw{gateway}.json.players.jsonl").read_text().splitlines()]
        assert len(rows) == summary["connections"]
        assert sum(r["successfulMoves"] for r in rows) == summary["business_succeeded"]
        for row in rows:
            assert row["playerId"] not in expected and row["successfulMoves"] > 0
            expected[row["playerId"]] = row
    assert set(expected) == {p["playerId"] for p in manifest}

    def snapshots_match():
        for player in manifest:
            pid = player["playerId"]
            state = sql(context, f"SELECT pos_x,pos_y,save_version,scene_id FROM {database}.player WHERE id={pid};")
            x, y, version, scene = map(int, state.split())
            if (x, y) != (expected[pid]["x"], expected[pid]["y"]) or version <= player["saveVersion"]:
                return False
            assert scene == player["sceneId"]
            assert sql(context, f"SELECT item_id,slot,quantity FROM {database}.player_item_stack WHERE player_id={pid};") == "101\t0\t9"
        return True
    until(snapshots_match)
    reloaded = []
    for gateway in (1, 2):
        config = (work / f"gw{gateway}.yaml").read_text()
        public_port = int(re.search(r"listenPort: (\d+)", config)[1])
        for scene in (gateway * 10,):
            player = next(p for p in manifest if p["gateway"] == gateway and p["sceneId"] == scene)
            pid = player["playerId"]
            with connect_player(public_port, pid) as sock:
                ack = fields(request(sock, 4, 2))
                assert ack.get(1, 0) == 0 and ack.get(2, 0)
                assert (ack.get(4, 0), ack.get(5, 0)) == (expected[pid]["x"], expected[pid]["y"])
                assert fields(request(sock, 5, 3)).get(1, 0) == 0
            reloaded.append(pid)
    result = dict(players=len(manifest), finalSnapshotsMatched=True, bagPreserved=True,
                  reloadedPlayers=reloaded)
    public_port = int(re.search(r"listenPort: (\d+)", (work / "gw1.yaml").read_text())[1])
    with connect_player(public_port, 9900001) as sock:
        rejected_enter(sock)
    wrong_scene = next(p for p in manifest if p["gateway"] == 2)
    with connect_player(public_port, wrong_scene["playerId"]) as sock:
        rejected_enter(sock)
    assert sql(context, f"SELECT COUNT(*) FROM {database}.player WHERE id=9900001;") == "0"
    assert snapshots_match(), "negative loads must not mutate persisted snapshots"
    result.update(missingPlayerRejected=True, sceneMismatchRejected=True)
    if context.get("negativeSave"):
        player = manifest[0]
        pid = player["playerId"]
        before = sql(context, f"SELECT pos_x,pos_y,save_version FROM {database}.player WHERE id={pid};")
        sql(context, f"CREATE TRIGGER {database}.reject_test_save BEFORE UPDATE ON {database}.player "
            "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected isolated save failure';")
        with connect_player(public_port, pid) as sock:
            ack = fields(request(sock, 4, 2))
            x, y = ack.get(4, 0), ack.get(5, 0)
            destination = x + 1 if x < 20 else x - 1
            moved = fields(request(sock, 2, 3, wire.walk_req(x, y, destination, y)))
            assert moved.get(1, 0) == 0 and moved.get(2) == destination
            assert fields(request(sock, 5, 4)).get(1, 0) == 0
        time.sleep(1.2)
        assert sql(context, f"SELECT pos_x,pos_y,save_version FROM {database}.player WHERE id={pid};") == before
        result.update(saveDeniedDatabaseUnchanged=True)
        (work / "EXPECT_DRAIN_FAILURE").write_text("scene10: injected UPDATE failure after acknowledged movement\n")
    (work / "persistence-verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS: {len(manifest)} database snapshots match client acknowledgements; 2 explicit-scene reloads passed")


def main(build_path, negative_save=False, connections=8, rps=10, duration=10, drain=5, queue_seconds=3,
         evidence_path=None, queue_diagnostics=False, recovery=0, frame_limit=1000):
    validate_options(argparse.Namespace(connections=connections, rps=rps, duration=duration,
                                        drain=drain, queue_seconds=queue_seconds, frame_limit=frame_limit))
    build = pathlib.Path(build_path).resolve()
    evidence = pathlib.Path(evidence_path) if evidence_path else build / ("persistence-load-" + str(time.time_ns()))
    evidence.mkdir(mode=0o700)
    processes, handles = [], []
    password = secrets.token_hex(24)
    success = False
    online_evidence = None
    with tempfile.TemporaryDirectory(prefix="clearmoon-persistence-load-") as tmp:
        work = pathlib.Path(tmp)
        mysql_port, redis_port = port(), port()
        assert mysql_port != redis_port
        context = dict(socket=str(work / "mysql.sock"), database="clearmoon_persistence_load",
                       password=password, players=seed_manifest(connections), negativeSave=negative_save)

        def start(name, args):
            handle = (work / (name + ".log")).open("w")
            handles.append(handle)
            process = subprocess.Popen(args, cwd=work, stdout=handle, stderr=subprocess.STDOUT)
            processes.append(process)
            return process

        try:
            subprocess.run(["mariadb-install-db", "--no-defaults", "--datadir=" + str(work / "data"),
                            "--auth-root-authentication-method=normal", "--skip-test-db"],
                           cwd=work, capture_output=True, timeout=60, check=True)
            start("mysql", [shutil.which("mariadbd"), "--no-defaults",
                            "--user=" + pwd.getpwuid(os.getuid()).pw_name,
                            "--datadir=" + str(work / "data"), "--socket=" + context["socket"],
                            "--pid-file=" + str(work / "mysql.pid"), "--port=" + str(mysql_port),
                            "--bind-address=127.0.0.1"])

            def ready():
                try:
                    return sql(context, "SELECT 1;") == "1"
                except AssertionError:
                    return False
            until(ready)
            database = context["database"]
            sql(context, f"CREATE DATABASE {database}; USE {database};\n" +
                (ROOT / "sql/001_player.sql").read_text() + "\n" +
                (ROOT / "sql/002_inventory.sql").read_text() + "\n" +
                f"CREATE USER 'persistence_test'@'127.0.0.1' IDENTIFIED BY '{password}';\n"
                f"GRANT SELECT,INSERT,UPDATE,DELETE ON {database}.* TO 'persistence_test'@'127.0.0.1';\n"
                "INSERT INTO item_definition(item_id,item_name) VALUES(101,'test potion');")
            for p in context["players"]:
                pid = p["playerId"]
                sql(context, f"USE {database}; INSERT INTO player(id,pos_x,pos_y,scene_id,save_version) "
                    f"VALUES({pid},{p['x']},{p['y']},{p['sceneId']},7);"
                    f"INSERT INTO player_container(player_id,container_kind,capacity,next_slot,container_version) VALUES({pid},1,64,1,7);"
                    f"INSERT INTO player_item_stack(player_id,container_kind,item_id,slot,quantity) VALUES({pid},1,101,0,9);")
            start("redis", ["redis-server", "--bind", "127.0.0.1", "--port", str(redis_port),
                            "--save", "", "--appendonly", "no"])
            until(lambda: tcp_ready(redis_port))
            config = make_zone_config((ZONE / "config/zoneConfig.yaml").read_text(),
                                      mysql_port, redis_port, 13145, password, database, 1)
            zone = work / "zone.yaml"
            zone.write_text(config)
            zone.chmod(0o600)
            context_path = work / "context.json"
            context_path.write_text(json.dumps(context))
            context_path.chmod(0o600)
            env = load_environment(frame_limit)
            env.pop("SCENE_QUEUE_DIAGNOSTICS", None)
            if queue_diagnostics:
                env["SCENE_QUEUE_DIAGNOSTICS"]="1"
            env.update(LOAD_CONNECTIONS_PER_GATEWAY=str(connections), LOAD_RPS=str(rps), LOAD_DURATION=str(duration), LOAD_DRAIN=str(drain),
                       LOAD_MAX_QUEUE_SECONDS=str(queue_seconds), LOAD_RECOVERY=str(recovery),
                       LOAD_PERSISTENCE_ZONE_CONFIG=str(zone), LOAD_PERSISTENCE_CONTEXT=str(context_path))
            with (work / "runner.log").open("w") as output:
                runner = subprocess.Popen(["bash", str(ZONE / "gatewayServer/tests/acceptance_multi_gateway_scene.sh"),
                    str(build / "bin/gatewayServer"), str(build / "bin/sceneServer"), str(ZONE),
                    str(build / "bin/gateway_load_client"), str(work / "run")],
                    env=env, cwd=work, stdout=output, stderr=subprocess.STDOUT)
                processes.append(runner)
                online_seen = False
                deadline = time.monotonic() + duration + drain + recovery + 100
                next_sample = 0
                while runner.poll() is None:
                    if time.monotonic() >= next_sample:
                        with (work / "resources.jsonl").open("a") as resource_file:
                            resource_file.write(json.dumps(sample_processes([p.pid for p in processes if p.poll() is None])) + "\n")
                        next_sample = time.monotonic() + 1
                    if time.monotonic() > deadline:
                        raise TimeoutError("persistence load exceeded 120 seconds")
                    barrier = work / "run/start-barrier/START"
                    if barrier.exists():
                        elapsed = time.monotonic() - int(barrier.read_text()) / 1000
                        if 2 < elapsed < 8:
                            count = sql(context, f"SELECT COUNT(*) FROM {database}.player WHERE save_version>7;")
                            if int(count) == len(context["players"]):
                                online_seen = True
                                if online_evidence is None:
                                    online_evidence = dict(elapsedMs=int(elapsed * 1000), advancedPlayers=int(count),
                                                           initialSaveVersion=7)
                    time.sleep(.1)
                assert runner.returncode == 0, "multi-gateway persistence runner failed"
                assert online_seen, "no proof of online periodic saving before load finished"
            success = True
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        success = False
            for handle in handles:
                handle.close()
            # 仅保留诊断产物，不保留数据库数据目录、套接字或凭据。
            paths = list(work.glob("*.log")) + list(work.glob("*.jsonl"))
            if (work / "run").exists():
                paths += [p for p in (work / "run").rglob("*")
                          if p.is_file() and p.suffix in (".log", ".json", ".jsonl", ".out")]
            for path in paths:
                target = evidence / path.relative_to(work)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(path.read_text(errors="replace").replace(password, "<redacted>"))
            (evidence / "summary.json").write_text(json.dumps(dict(passed=success, players=connections*2,
                connectionsPerGateway=connections, rpsPerConnection=rps, durationSec=duration, drainSec=drain,
                queueSeconds=queue_seconds, gatewayFrameLimit=frame_limit,
                queueDiagnostics=queue_diagnostics,
                negativeSave=negative_save, onlineSavingVerified=online_evidence is not None,
                onlineSaveEvidence=online_evidence), indent=2) + "\n")
            print(f"Evidence: {evidence}", flush=True)
    assert success
    print("PASS: isolated dual-gateway persistence acceptance")
    return evidence


def tcp_ready(number):
    try:
        with socket.create_connection(("127.0.0.1", number), timeout=.2):
            return True
    except OSError:
        return False


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--verify":
        verify(sys.argv[2], sys.argv[3])
    else:
        args = parse_args()
        main(args.build, args.negative_save, args.connections, args.rps, args.duration, args.drain, args.queue_seconds,
             queue_diagnostics=args.queue_diagnostics, frame_limit=args.frame_limit)