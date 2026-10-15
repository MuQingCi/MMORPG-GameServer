#!/usr/bin/env python3
"""需显式执行的隔离持久化验收，依赖 MariaDB 和 Redis 工具。

用法：python3 persistence_e2e.py /absolute/path/to/Zone/build-acceptance
不连接现有数据库；退出时删除临时数据和凭据。
"""
import importlib.util
import os
import pathlib
import pwd
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
ZONE = ROOT / "Zone"
spec = importlib.util.spec_from_file_location(
    "wire", ZONE / "gatewayServer/tests/real_scene_client.py")
wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def until(fn, timeout=15):
    deadline = time.monotonic() + timeout
    while True:
        value = fn()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise TimeoutError("acceptance condition timed out")
        time.sleep(0.05)


def fields(body):
    values = {}
    index = 0
    while index < len(body):
        def varint():
            nonlocal index
            result = shift = 0
            while index < len(body) and shift < 70:
                byte = body[index]
                index += 1
                result |= (byte & 127) << shift
                if byte < 128:
                    return result
                shift += 7
            raise AssertionError("malformed protobuf varint")
        tag = varint()
        assert tag & 7 == 0, "unexpected noninteger ack field"
        values[tag >> 3] = varint()
    return values


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    work = pathlib.Path(tempfile.mkdtemp(prefix="clearmoon-persistence-e2e-"))
    os.chmod(work, 0o700)
    processes, handles, checks = [], [], []
    password = secrets.token_hex(24)
    mysql_port, redis_port, public_port, private_port = port(), port(), port(), port()
    assert len({mysql_port, redis_port, public_port, private_port}) == 4

    def start(name, command, env=None):
        handle = (work / (name + ".stdout")).open("wb")
        handles.append(handle)
        process = subprocess.Popen(command, cwd=work, stdout=handle,
                                   stderr=subprocess.STDOUT, env=env)
        processes.append(process)
        return process

    def stop(process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise AssertionError("process failed graceful shutdown")

    def sql(statement):
        result = subprocess.run([
            "mariadb", "--no-defaults", "--protocol=socket",
            "--socket=" + str(work / "mysql.sock"), "--user=root", "--batch", "--skip-column-names"],
            input=statement, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        if result.returncode:
            raise AssertionError("isolated SQL failed: " + result.stderr.replace(password, "<redacted>"))
        return result.stdout.strip()

    database = "clearmoon_persistence_e2e"

    def state(pid=4242):
        row = sql(f"SELECT pos_x,pos_y,save_version FROM {database}.player WHERE id={pid};")
        return tuple(map(int, row.split()))

    def bag(pid=4242):
        return sql(f"SELECT item_id,slot,quantity FROM {database}.player_item_stack "
                   f"WHERE player_id={pid} AND container_kind=1 ORDER BY slot;")

    def request(sock, method, rid, body=b""):
        sock.sendall(wire.client_frame(wire.KIND_REQUEST, 3, method, rid, body))
        for _ in range(16):
            frame = wire.read_frame(sock)
            assert frame is not None, "client connection closed"
            if frame[3] == rid:
                assert frame[:4] == (wire.KIND_RESPONSE, 3, method, rid), frame
                return frame[4]
        raise AssertionError("request response missing")

    def login(pid, expected):
        def attempt():
            sock = socket.create_connection(("127.0.0.1", public_port), timeout=2)
            sock.settimeout(8)
            try:
                import struct
                sock.sendall(wire.client_frame(wire.KIND_CONTROL, 1, 1, 1,
                                               struct.pack("<QQ", pid, 998877)))
                auth = wire.read_frame(sock)
                assert auth and auth[:4] == (wire.KIND_CONTROL, 1, 2, 1), auth
                sock.sendall(wire.client_frame(wire.KIND_REQUEST, 3, 4, 2))
                frame = wire.read_frame(sock)
                if frame and frame[0] == wire.KIND_ERROR and b"unavailable" in frame[4]:
                    sock.close()
                    return None
                assert frame and frame[:4] == (wire.KIND_RESPONSE, 3, 4, 2), frame
                ack = fields(frame[4])
                assert ack.get(1, 0) == 0 and ack.get(2, 0), frame
                assert (ack.get(4, 0), ack.get(5, 0)) == expected, ack
                return sock
            except BaseException:
                sock.close()
                raise
        return until(attempt)

    def move(sock, source, destination, rid):
        ack = fields(request(sock, 2, rid, wire.walk_req(*source, *destination)))
        assert ack.get(1, 0) == 0 and (ack.get(2, 0), ack.get(3, 0)) == destination, ack

    try:
        init = subprocess.run(["mariadb-install-db", "--no-defaults", "--datadir=" + str(work / "data"),
                               "--auth-root-authentication-method=normal", "--skip-test-db"],
                              cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        assert init.returncode == 0, "isolated MariaDB initialization failed"
        start("mysql", [shutil.which("mariadbd") or "/usr/sbin/mariadbd", "--no-defaults",
                        "--user=" + pwd.getpwuid(os.getuid()).pw_name,
                        "--datadir=" + str(work / "data"), "--socket=" + str(work / "mysql.sock"),
                        "--pid-file=" + str(work / "mysql.pid"), "--port=" + str(mysql_port),
                        "--bind-address=127.0.0.1", "--log-error=" + str(work / "mysql.log")])

        def ready():
            try:
                return sql("SELECT 1;") == "1"
            except AssertionError:
                return False
        until(ready)
        sql(f"CREATE DATABASE {database}; USE {database};\n" +
            (ROOT / "sql/001_player.sql").read_text() + "\n" +
            (ROOT / "sql/002_inventory.sql").read_text() + "\n" +
            f"CREATE USER 'persistence_test'@'127.0.0.1' IDENTIFIED BY '{password}';\n"
            f"GRANT SELECT,INSERT,UPDATE,DELETE ON {database}.* TO 'persistence_test'@'127.0.0.1';\n"
            "INSERT INTO item_definition(item_id,item_name) VALUES(101,'test potion');\n"
            "INSERT INTO player(id,pos_x,pos_y,save_version) VALUES(4242,5,6,7),(4243,7,8,7);\n"
            "INSERT INTO player_container(player_id,container_kind,capacity,next_slot,container_version) "
            "VALUES(4242,1,64,1,7),(4243,1,64,1,7);\n"
            "INSERT INTO player_item_stack(player_id,container_kind,item_id,slot,quantity) "
            "VALUES(4242,1,101,0,9),(4243,1,101,0,3);")
        env = os.environ.copy()
        env["PERSISTENCE_TEST_PASSWORD"] = password
        probe = subprocess.run([str(build / "sceneServer/tests/persistence_dao_probe"), str(mysql_port)],
                               env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        assert probe.returncode == 0, probe.stdout
        checks.append(probe.stdout.strip())
        start("redis", ["redis-server", "--bind", "127.0.0.1", "--port", str(redis_port),
                        "--save", "", "--appendonly", "no"])
        (work / "gateway.yaml").write_text(f"""gateway:
    zoneId: 1
    gatewayId: 3
    publicConfig:
        listenAddr: 127.0.0.1
        listenPort: {public_port}
    privateConfig:
        listenAddr: 127.0.0.1
        listenPort: {private_port}
        allowedServices:
            - 8
    eventThreadNum: 2
    clientToken: 998877
    defaultServiceId: 8
    maxClientSessions: 64
    maxPendingRoutes: 256
    routeTtlMs: 10000
    logLevel: info
""")
        zone_config = (ZONE / "config/zoneConfig.yaml").read_text()
        zone_config = zone_config.replace("port: 3306", f"port: {mysql_port}")
        zone_config = zone_config.replace("port: 6379", f"port: {redis_port}")
        zone_config = zone_config.replace("user: root", "user: persistence_test")
        zone_config = zone_config.replace('password: ""', f'password: "{password}"', 1)
        zone_config = zone_config.replace("dbName: game", "dbName: " + database)
        zone_config = zone_config.replace("127.0.0.1:13145", f"127.0.0.1:{private_port}")
        (work / "zone.yaml").write_text(zone_config)
        os.chmod(work / "zone.yaml", 0o600)
        scene_config = (ZONE / "sceneServer/config/sceneConfig.yaml").read_text()
        scene_config = scene_config.replace("luaDir: ./lua_script", f"luaDir: {ZONE}/sceneServer/lua_script")
        scene_config = scene_config.replace("flushIntervalMs: 30000", "flushIntervalMs: 500")
        scene_config += f"\n    mapPath: {ZONE}/sceneServer/config/map.yaml\n"
        (work / "scene.yaml").write_text(scene_config)
        start("gateway", [str(build / "bin/gatewayServer"), str(work / "gateway.yaml")])

        def gateway_ready():
            try:
                with socket.create_connection(("127.0.0.1", private_port), timeout=0.2):
                    return True
            except OSError:
                return False
        until(gateway_ready)

        def scene_start(name):
            return start(name, [str(build / "bin/sceneServer"), str(work / "zone.yaml"), str(work / "scene.yaml")])

        scene = scene_start("scene1")
        sock = login(4242, (5, 6))
        checks.append("PASS real gateway database spawn (5,6), not default origin")
        move(sock, (5, 6), (10, 20), 3)
        assert fields(request(sock, 5, 4)).get(1, 0) == 0
        # 登出后立即重新鉴权，不先等待 SQL 完成。
        import struct
        sock.sendall(wire.client_frame(wire.KIND_CONTROL, 1, 1, 5, struct.pack("<QQ", 4242, 998877)))
        assert wire.read_frame(sock)[:4] == (wire.KIND_CONTROL, 1, 2, 5)
        ack = fields(request(sock, 4, 6))
        assert ack.get(1, 0) == 0 and (ack.get(4, 0), ack.get(5, 0)) == (10, 20), ack
        assert until(lambda: state() if state()[:2] == (10, 20) and state()[2] > 7 else None)
        assert bag() == "101\t0\t9"
        checks.append("PASS offline save, immediate rebind/load latest state, bag preserved")
        version = state()[2]
        move(sock, (10, 20), (11, 20), 7)
        until(lambda: state()[:2] == (11, 20) and state()[2] > version)
        checks.append("PASS online periodic snapshot confirmed in real database")
        version = state()[2]
        move(sock, (11, 20), (12, 20), 8)
        stop(scene)
        assert scene.returncode == 0, "scene graceful shutdown failed"
        assert state()[:2] == (12, 20) and state()[2] > version
        assert bag() == "101\t0\t9"
        sock.close()
        checks.append("PASS graceful shutdown persisted final movement and bag")
        scene = scene_start("scene2")
        sock = login(4242, (12, 20))
        second = login(4243, (7, 8))
        move(second, (7, 8), (8, 9), 3)
        second.close()
        until(lambda: state(4243)[:2] == (8, 9) and state(4243)[2] > 7)
        assert bag(4243) == "101\t0\t3"
        move(sock, (12, 20), (13, 20), 9)
        sock.close()
        until(lambda: state()[:2] == (13, 20))
        checks.append("PASS scene restart load and per-client TCP disconnect persistence")
        stop(scene)
        logs = "\n".join(p.read_text(errors="replace") for p in (work / "logs").rglob("*.log"))
        assert "shutdown persistence drain incomplete" not in logs
        assert "shutdown not durably drained" not in logs
        summary = "\n".join(checks) + "\nAll isolated persistence checks passed.\n"
        (build / "persistence_e2e_summary.txt").write_text(summary)
        print(summary, end="")
    except BaseException:
        for p in work.glob("*.stdout"):
            print(f"--- {p.name} ---\n" + p.read_text(errors="replace")[-6000:].replace(password, "<redacted>"), file=sys.stderr)
        logs = "\n".join(p.read_text(errors="replace") for p in (work / "logs").rglob("*.log"))
        print(logs[-12000:].replace(password, "<redacted>"), file=sys.stderr)
        raise
    finally:
        for process in reversed(processes):
            try:
                stop(process)
            except AssertionError:
                pass
        for handle in handles:
            handle.close()
        shutil.rmtree(work)


if __name__ == "__main__":
    main()