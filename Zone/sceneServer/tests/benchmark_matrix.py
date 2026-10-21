#!/usr/bin/env python3
"""可复现、顺序执行的 Release 基准，使用标准库及隔离的 MariaDB/Redis。

每次调用保留失败用例；--resume 只跳过已有完整 result.json 的用例。
不连接生产数据库，不删除旧验收产物。
"""
import argparse
import contextlib
import csv
import gzip
import hashlib
import itertools
import json
import math
import os
import pathlib
import platform
import re
import secrets
import shutil
import signal
import subprocess
import tempfile
import time
import traceback

from persistence_e2e import ROOT, ZONE, fields, make_zone_config, port, until
from persistence_load_e2e import sql, connect_player, request, tcp_ready
from load_resources import sample_processes


def cases():
    return [dict(gateways=g, scenes=s, connections=c, rps=r, persistence=p, duration=d)
            for g, s, c, r, p, d in itertools.product((1, 2), (1, 2),
                (64, 128, 256, 512, 1024), (10, 20, 30), (False, True), (10, 30, 60, 120))]


def frame_limit(connections, rps):
    return 100000 if connections * rps > 10000 else 10000


def approved_cases():
    return [dict(gateways=3, scenes=2, connections=c, rps=20, persistence=True, duration=d)
            for c, d in itertools.product((128, 256, 512), (30, 60))]


def distribute(total, gateways):
    return [total // gateways + (i < total % gateways) for i in range(gateways)]


def player_ids(base, total, gateways):
    return [base + g*2000 + i for g, count in enumerate(distribute(total, gateways)) for i in range(count)]


def exact_case_set(selected, actual):
    expected = [identity(c) for c in selected]
    return len(expected) == len(set(expected)) == len(actual) and set(expected) == set(actual)


def percentiles(values):
    values = sorted(values)
    return {name: values[math.ceil(len(values)*p)-1] if values else None
            for name, p in (('p50', .5), ('p90', .9), ('p99', .99))}


def replace_scalar(text, key, value):
    text, count = re.subn(r'(?m)^(\s*' + re.escape(key) + r':).*$',
                          lambda m: m[1] + ' ' + str(value), text)
    if count != 1:
        raise ValueError(f'{key}: expected exactly one scalar, found {count}')
    return text


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    temporary.replace(path)


def identity(case):
    return 'g{gateways}s{scenes}-c{connections}-r{rps}-p{persistence}-d{duration}'.format(**case)


def interrupted(signum, frame):
    raise KeyboardInterrupt(f'benchmark interrupted by signal {signum}')


class Runner:
    def __init__(self, build, work):
        self.build, self.work = build, work
        self.processes, self.handles = [], []

    def start(self, name, args):
        handle = (self.work / (name + '.stdout.log')).open('w')
        self.handles.append(handle)
        process = subprocess.Popen(args, cwd=self.work, stdout=handle, stderr=subprocess.STDOUT)
        self.processes.append((name, process))
        return process

    def stop(self, selected=None):
        statuses = {}
        for name, process in reversed(self.processes if selected is None else selected):
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    statuses[name] = 'forced-kill'
            statuses.setdefault(name, process.returncode)
        return statuses

    def sample(self, output):
        row = sample_processes([p.pid for _, p in self.processes if p.poll() is None])
        names = {p.pid: name for name, p in self.processes}
        for process in row['processes']:
            process['role'] = names.get(process['pid'], 'child')
            proc = pathlib.Path('/proc') / str(process['pid'])
            try:
                process['pssBytes'] = int(re.search(r'(?m)^Pss:\s+(\d+)',
                    (proc / 'smaps_rollup').read_text())[1]) * 1024
                process['io'] = {k: int(v) for k, v in
                    (line.split(':') for line in (proc / 'io').read_text().splitlines())}
            except (OSError, TypeError):
                process['pssBytes'] = None
            if process['role'].startswith(('gw', 'scene')):
                process['threads'] = []
                for task in (proc / 'task').glob('*/stat'):
                    try:
                        text = task.read_text(); values = text[text.rindex(')')+2:].split()
                        process['threads'].append(dict(tid=int(task.parent.name),
                            cpuTicks=int(values[11])+int(values[12])))
                    except (OSError, ValueError, IndexError):
                        continue
        row['networkNamespace'] = pathlib.Path('/proc/net/dev').read_text()
        output.write(json.dumps(row) + '\n')
        output.flush()


def seed(context, players):
    statements = [f'USE {context["database"]};']
    for p in players:
        statements += [f"INSERT INTO player(id,pos_x,pos_y,scene_id,save_version) VALUES({p['playerId']},{p['x']},6,{p['sceneId']},7);",
            f"INSERT INTO player_container(player_id,container_kind,capacity,next_slot,container_version) VALUES({p['playerId']},1,64,1,7);",
            f"INSERT INTO player_item_stack(player_id,container_kind,item_id,slot,quantity) VALUES({p['playerId']},1,101,0,9);"]
    sql(context, '\n'.join(statements))


def lifecycle_client_args():
    return ['--warmup', '0', '--idle-before', '15', '--hold-after', '20',
            '--preparation-timeout', '30', '--stagger']


def initialize_database(datadir, logfile, executable='mariadb-install-db'):
    with pathlib.Path(logfile).open('w') as output:
        subprocess.run([executable, '--no-defaults', '--datadir=' + str(datadir),
                        '--auth-root-authentication-method=normal', '--skip-test-db',
                        '--innodb-log-file-size=16777216'],
                       stdout=output, stderr=subprocess.STDOUT, check=True, timeout=60)


def run_case(build, work, case, index, capacity=False, frozen_players=None, preparation_only=False, lifecycle=False):
    if lifecycle and (case != dict(gateways=1, scenes=1, connections=128, rps=1, persistence=True, duration=30) or capacity or preparation_only):
        raise ValueError('lifecycle profile requires the single approved 128x1 case')
    work.mkdir(parents=True, mode=0o700)
    write_json(work / 'case.json', case)
    runner = Runner(build, work)
    result = dict(case=case, passed=False, startedAt=time.strftime('%Y-%m-%dT%H:%M:%S%z'))
    password = secrets.token_hex(24)
    resources = (work / 'resources.jsonl').open('w')
    temporary = tempfile.mkdtemp(prefix='matrix-db-')
    try:
        with contextlib.nullcontext(temporary):
            dbwork = pathlib.Path(temporary)
            pubs, privs = [], []
            allocated = set()
            def unique_port():
                value = port()
                while value in allocated:
                    value = port()
                allocated.add(value)
                return value
            mysql, redis = unique_port(), unique_port()
            context = dict(socket=str(dbwork / 'mysql.sock'), database='matrix_test', password=password)
            if case['persistence']:
                initialize_database(dbwork / 'data', work / 'database-bootstrap.log')
                runner.start('mariadb', ['mariadbd', '--no-defaults', '--user=' + __import__('pwd').getpwuid(os.getuid()).pw_name,
                    '--datadir=' + str(dbwork / 'data'), '--socket=' + context['socket'],
                    '--pid-file=' + str(dbwork / 'mysql.pid'), '--port=' + str(mysql), '--bind-address=127.0.0.1',
                    '--innodb-log-file-size=16777216'])
                def ready():
                    try:
                        return sql(context, 'SELECT 1;') == '1'
                    except AssertionError:
                        return False
                until(ready, 30)
                sql(context, 'CREATE DATABASE matrix_test; USE matrix_test;\n' +
                    (ROOT / 'sql/001_player.sql').read_text() + (ROOT / 'sql/002_inventory.sql').read_text() +
                    f"CREATE USER 'persistence_test'@'127.0.0.1' IDENTIFIED BY '{password}';"
                    "GRANT SELECT,INSERT,UPDATE,DELETE ON matrix_test.* TO 'persistence_test'@'127.0.0.1';"
                    "INSERT INTO item_definition(item_id,item_name) VALUES(101,'test potion');")
                runner.start('redis', ['redis-server', '--bind', '127.0.0.1', '--port', str(redis), '--save', '', '--appendonly', 'no'])
                until(lambda: tcp_ready(redis))
            limit = frame_limit(case['connections'], case['rps'])
            for gateway in range(1, case['gateways'] + 1):
                public, private = unique_port(), unique_port()
                pubs.append(public); privs.append(private)
                text = f'''gateway:
    zoneId: 1
    gatewayId: {gateway}
    backendChannels: 2
    publicConfig:
        listenAddr: 127.0.0.1
        listenPort: {public}
    privateConfig:
        listenAddr: 127.0.0.1
        listenPort: {private}
        allowedServices:
            - 8
    eventThreadNum: 2
    clientToken: 998877
    defaultServiceId: 8
    defaultSceneId: 0
    maxClientSessions: 2048
    maxPendingRoutes: 20000
    routeTtlMs: 15000
    maxFramesPerSec: {limit}
    logLevel: info
    logDir: {work}/logs
    logName: gw{gateway}
    metricsPath: {work}/gw{gateway}.metrics.json
'''
                path = work / f'gw{gateway}.yaml'; path.write_text(text)
                runner.start(f'gw{gateway}', [str(build / 'bin/gatewayServer'), str(path)])
            until(lambda: all(tcp_ready(p) for p in privs))
            zone = make_zone_config((ZONE / 'config/zoneConfig.yaml').read_text(), mysql, redis, privs[0], password, 'matrix_test', 1)
            zone = replace_scalar(zone, 'enable', str(case['persistence']).lower())
            zone = replace_scalar(zone, 'backendChannels', 2)
            zone = replace_scalar(zone, 'dir', str(work / 'logs'))
            zone = re.sub(r'(?m)^        - \d+=127\.0\.0\.1:\d+.*$',
                          '\n'.join(f'        - {i+1}=127.0.0.1:{p}' for i, p in enumerate(privs)), zone)
            zonepath = work / 'zone.yaml'; zonepath.write_text(zone); zonepath.chmod(0o600)
            for scene in range(1, case['scenes'] + 1):
                text = (ZONE / 'sceneServer/config/sceneConfig.yaml').read_text()
                for key, value in dict(sceneId=scene*10, luaDir=ZONE / 'sceneServer/lua_script',
                    maxFramesPerSec=limit, flushIntervalMs=30000 if lifecycle else 1000).items():
                    text = replace_scalar(text, key, value)
                text += f'\n    mapPath: {ZONE}/sceneServer/config/map.yaml\n    metricsPath: {work}/scene{scene}.metrics.json\n'
                path = work / f'scene{scene}.yaml'; path.write_text(text)
                runner.start(f'scene{scene}', [str(build / 'bin/sceneServer'), str(zonepath), str(path)])
            def backends_ready():
                try:
                    return all(json.loads((work / f'gw{g}.metrics.json').read_text())['backendReady'] == case['scenes']*2
                               for g in range(1, case['gateways']+1))
                except (OSError, ValueError, KeyError):
                    return False
            until(backends_ready, 45)
            if capacity:
                time.sleep(15)
            base = 10000000 + index * 10000
            manifest = []
            # 探测实际自动布置，不依赖 unordered_map 的遍历顺序。
            # 探测数据行与会话不计入测量区间。
            mapping = {}
            if case['persistence']:
                for gateway, public in enumerate(pubs, 1):
                    for parity in range(case['scenes']):
                        pid = base + 9000 + gateway*10 + parity
                        seed(context, [dict(playerId=pid, x=5, sceneId=10)])
                        for sceneid in (10, 20)[:case['scenes']]:
                            sql(context, f'UPDATE matrix_test.player SET scene_id={sceneid} WHERE id={pid};')
                            with connect_player(public, pid) as sock:
                                body = request(sock, 4, 2)
                                if not body.startswith(b'\x08\xf2\x07\x12'):
                                    ack = fields(body)
                                    assert ack.get(1, 0) == 0 and ack.get(2, 0)
                                    assert fields(request(sock, 5, 3)).get(1, 0) == 0
                                    mapping[gateway, pid % case['scenes']] = sceneid
                                    break
                            time.sleep(.15)
                        assert (gateway, pid % case['scenes']) in mapping
            if frozen_players is not None:
                from busy_placement import validate
                actual = [dict(playerId=base+(gateway-1)*2000+i, gateway=gateway,
                               sceneId=mapping.get((gateway, (base+(gateway-1)*2000+i) % case['scenes'])), x=5+i)
                          for gateway in range(1, case['gateways']+1)
                          for i in range(distribute(case['connections'], case['gateways'])[gateway-1])]
                placement = validate(frozen_players, actual)
                write_json(work/'placement-verification.json', placement)
                write_json(work/'actual-players.json', actual)
                assert placement['matched'], 'frozen placement mismatch; business traffic forbidden'
            if preparation_only:
                assert frozen_players is not None, 'preparation requires frozen players'
                seed(context, frozen_players)
                admitted = []
                for player in frozen_players:
                    with connect_player(pubs[player['gateway']-1], player['playerId']) as sock:
                        body = request(sock, 4, 2)
                        ack = fields(body)
                        assert ack.get(1, 0) == 0 and ack.get(2, 0), 'frozen player admission failed'
                        assert [ack.get(4, 0), ack.get(5, 0)] == [player['x'], player.get('y', 6)]
                        assert fields(request(sock, 5, 3)).get(1, 0) == 0, 'preparation logout failed'
                    admitted.append(player['playerId'])
                    write_json(work/'admitted-players.json', admitted)
                result['businessTraffic'] = False
                result['shutdown'] = runner.stop([(n, p) for n, p in runner.processes if n.startswith('scene')])
                result['passed'] = all(v == 0 for v in result['shutdown'].values())
                return result
            barrier = work / 'start-barrier'; barrier.mkdir()
            clients = []
            for gateway, public in enumerate(pubs, 1):
                connections = distribute(case['connections'], case['gateways'])[gateway-1]
                playerbase = base + (gateway-1)*2000
                for i in range(connections):
                    pid = playerbase+i
                    manifest.append(dict(playerId=pid, gateway=gateway,
                        sceneId=mapping.get((gateway, pid % case['scenes'])), x=5+i))
                if case['persistence']:
                    seed(context, manifest[-connections:])
                args = [str(build / 'bin/gateway_load_client'), '--port', str(public), '--connections', str(connections),
                    '--rps', str(case['rps']), '--duration', str(case['duration']), '--drain', '10', '--scene',
                    '--player-base', str(playerbase), '--barrier-dir', str(barrier),
                    '--gateway-metrics', str(work / f'gw{gateway}.metrics.json'),
                    '--metrics-output', str(work / f'metrics-gw{gateway}.jsonl'),
                    '--output', str(work / f'load-gw{gateway}.json')]
                if capacity:
                    args += ['--warmup','20','--preparation-timeout','120','--stagger']
                if lifecycle:
                    args += lifecycle_client_args()
                for scene in range(1, case['scenes']+1):
                    args += ['--scene-metrics' if scene == 1 else '--scene-metrics-extra', str(work / f'scene{scene}.metrics.json')]
                clients.append(runner.start(f'client{gateway}', args))
            write_json(work / 'players.json', manifest)
            def all_ready():
                assert all(p.poll() is None for p in clients), 'client preparation failed'
                return len(list(barrier.glob('*.ready'))) == case['connections']
            until(all_ready, 120 if capacity else 60)
            warmup_start = time.monotonic() + (5 if capacity else .5)
            start = warmup_start + (15 if lifecycle else (20 if capacity else 0))
            (barrier / 'START').write_text(str(int(warmup_start*1000)))
            result['loadStartMonotonicMs'] = int(start*1000)
            deadline = start + case['duration'] + 35
            online_evidence = None
            while any(p.poll() is None for p in clients):
                runner.sample(resources)
                elapsed = time.monotonic() - start
                if case['persistence'] and online_evidence is None and 2 < elapsed < case['duration']:
                    ids = ','.join(str(p['playerId']) for p in manifest)
                    advanced = int(sql(context, f'SELECT COUNT(*) FROM matrix_test.player WHERE save_version>7 AND id IN ({ids});'))
                    if advanced == case['connections']:
                        online_evidence = dict(elapsedSec=elapsed, advancedPlayers=advanced)
                assert time.monotonic() < deadline, 'load deadline exceeded'
                assert all(p.poll() is None for name, p in runner.processes if not name.startswith('client')), 'service exited'
                time.sleep(1)
            runner.sample(resources)
            result['clientExitCodes'] = [p.returncode for p in clients]
            if lifecycle:
                result['postLogoutStartMonotonicMs'] = int(time.monotonic()*1000)
                for _ in range(20):
                    runner.sample(resources)
                    time.sleep(1)
            result['onlineSaveEvidence'] = online_evidence
            if case['persistence'] and not capacity:
                expected = {}
                for path in work.glob('load-gw*.json.players.jsonl'):
                    for line in path.read_text().splitlines():
                        row = json.loads(line); expected[row['playerId']] = row
                assert len(expected) == len(manifest), 'missing acknowledged logout positions'
                def snapshots():
                    rows = sql(context, 'SELECT id,pos_x,pos_y,save_version,scene_id FROM matrix_test.player;')
                    states = {int(v[0]): list(map(int, v[1:])) for v in (line.split() for line in rows.splitlines())}
                    return all(states[p['playerId']][:2] == [expected[p['playerId']]['x'], expected[p['playerId']]['y']]
                        and states[p['playerId']][2] > 7 and states[p['playerId']][3] == p['sceneId'] for p in manifest)
                until(snapshots, 20)
                (work / 'final-db.tsv').write_text(sql(context, 'SELECT id,pos_x,pos_y,save_version,scene_id FROM matrix_test.player ORDER BY id;') + '\n')
                bagcount = int(sql(context, 'SELECT COUNT(*) FROM matrix_test.player_item_stack WHERE item_id=101 AND slot=0 AND quantity=9;'))
                bags = sql(context, 'SELECT player_id,item_id,slot,quantity FROM matrix_test.player_item_stack ORDER BY player_id;')
                (work / 'final-bags.tsv').write_text(bags + '\n')
                bagstates = {int(v[0]): list(map(int, v[1:])) for v in (line.split() for line in bags.splitlines())}
                assert all(bagstates.get(p['playerId']) == [101, 0, 9] for p in manifest)
                reloads = []
                for gateway, public in enumerate(pubs, 1):
                    for sceneid in range(10, case['scenes']*10+1, 10):
                        p = next(p for p in manifest if p['gateway'] == gateway and p['sceneId'] == sceneid)
                        pid = p['playerId']
                        with connect_player(public, pid) as sock:
                            ack = fields(request(sock, 4, 2))
                            assert ack.get(1, 0) == 0 and [ack.get(4, 0), ack.get(5, 0)] == [expected[pid]['x'], expected[pid]['y']]
                            assert fields(request(sock, 5, 3)).get(1, 0) == 0
                        reloads.append(pid)
                write_json(work / 'persistence-verification.json', dict(finalSnapshotsMatched=True, bagRows=bagcount, reloadedPlayers=reloads))
            result['shutdown'] = runner.stop([(n, p) for n, p in runner.processes if n.startswith('scene')])
            result['passed'] = all(p.returncode == 0 for p in clients) and all(v == 0 for v in result['shutdown'].values())
            if case['persistence'] and online_evidence is None:
                result['passed'] = False
                result['error'] = 'online periodic save proof missing'
            # 在 TemporaryDirectory 删除数据目录之前停止隔离数据库。
            runner.stop()
    except Exception:
        result['error'] = traceback.format_exc().replace(password, '<redacted>')
    finally:
        result['cleanupExitCodes'] = runner.stop()
        shutil.rmtree(temporary)
        resources.close()
        for handle in runner.handles:
            handle.close()
        zone = work / 'zone.yaml'
        if zone.exists():
            zone.write_text(zone.read_text().replace(password, '<redacted>'))
        result['finishedAt'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
        write_json(work / 'result.json', result)
    return result


def aggregate(batch):
    rows = []
    for path in sorted(batch.glob('*/result.json')):
        cache = path.parent / 'case-summary.json'
        if cache.exists():
            rows.append(json.loads(cache.read_text()))
            continue
        result = json.loads(path.read_text())
        case, work = result['case'], path.parent
        clients = [json.loads(p.read_text()) for p in work.glob('load-gw*.json')]
        samples = []
        for p in work.glob('*.latency-us.txt*'):
            with (gzip.open(p, 'rt') if p.suffix == '.gz' else p.open()) as source:
                samples.extend(int(line) for line in source)
        totals = {key: sum(c.get(key, 0) for c in clients) for key in
                  ('load_attempted', 'sent', 'completed', 'business_succeeded', 'errors', 'unresolved', 'schedule_delay_us', 'logout_succeeded')}
        row = dict(id=work.name, **case, passed=result['passed'], **totals,
            targetRps=case['connections']*case['rps'],
            actualSendRps=totals['sent']/case['duration'],
            successfulRps=totals['business_succeeded']/case['duration'],
            latencySamples=len(samples), **{k+'Us': v for k, v in percentiles(samples).items()})
        row['accountingValid'] = bool(clients) and totals['load_attempted'] == totals['completed'] + totals['unresolved']
        row['latencyCountValid'] = len(samples) == totals['business_succeeded']
        row['meanScheduleDelayUs'] = sum(c.get('schedule_delay_us', 0) for c in clients) / max(totals['sent'], 1)
        row['maxScheduleDelayUs'] = max((c.get('schedule_delay_max_us', 0) for c in clients), default=0)
        # 窗口成功吞吐不包含排空期间收到的响应。
        boundary_success = 0
        for p in work.glob('metrics-gw*.jsonl'):
            boundaries = [json.loads(line) for line in p.read_text().splitlines()]
            boundary = next((r for r in boundaries if r['elapsedMs'] == case['duration']*1000), None)
            if boundary:
                boundary_success += boundary['client_business_succeeded']
        row['loadWindowSuccessRps'] = boundary_success / case['duration']
        row['successRate'] = totals['business_succeeded'] / max(totals['load_attempted'], 1)
        row['targetAttainment'] = totals['sent'] / (case['duration'] * case['connections'] * case['rps'])
        metric_summary = {}
        for p in work.glob('metrics-gw*.jsonl'):
            observations = [json.loads(line) for line in p.read_text().splitlines()]
            keys = {k for r in observations for k in r if k.startswith(('gateway_', 'scene'))}
            metric_summary[p.name] = {k: dict(first=observations[0].get(k), last=observations[-1].get(k),
                peak=max((r[k] for r in observations if isinstance(r.get(k), (int, float))), default=None))
                for k in keys}
        write_json(work / 'metrics-summary.json', metric_summary)
        resource_rows = [json.loads(line) for line in (work / 'resources.jsonl').read_text().splitlines()]
        start = result.get('loadStartMonotonicMs', 0)
        window = [r for r in resource_rows if start <= r['monotonicMs'] <= start + case['duration']*1000]
        resource_summary = {}
        for role in sorted({p['role'] for r in window for p in r['processes']}):
            observations = [(r, p) for r in window for p in r['processes'] if p['role'] == role]
            cpus, reads, writes = [], [], []
            for (a, p), (b, q) in zip(observations, observations[1:]):
                if (p['pid'], p['startTicks']) != (q['pid'], q['startTicks']):
                    continue
                seconds = (b['monotonicMs'] - a['monotonicMs']) / 1000
                if seconds <= 0:
                    continue
                cpus.append(100 * (q['cpuTicks'] - p['cpuTicks']) / b['clockTicks'] / seconds)
                for key, values in (('read_bytes', reads), ('write_bytes', writes)):
                    if key in p.get('io', {}) and key in q.get('io', {}):
                        values.append((q['io'][key] - p['io'][key]) / seconds)
            resource_summary[role] = dict(samples=len(observations),
                cpuMeanPct=sum(cpus)/len(cpus) if cpus else None, cpuPeakPct=max(cpus, default=None),
                rssMeanBytes=sum(p['rssBytes'] for _, p in observations)/len(observations),
                rssPeakBytes=max(p['rssBytes'] for _, p in observations),
                pssPeakBytes=max((p['pssBytes'] for _, p in observations if p.get('pssBytes') is not None), default=None),
                readMeanBytesSec=sum(reads)/len(reads) if reads else None,
                writeMeanBytesSec=sum(writes)/len(writes) if writes else None)
        write_json(work / 'resource-summary.json', resource_summary)
        row['serviceCpuMeanPct'] = sum(v['cpuMeanPct'] or 0 for k, v in resource_summary.items() if k.startswith(('gw', 'scene')))
        row['serviceRssPeakSumBytes'] = sum(v['rssPeakBytes'] for k, v in resource_summary.items() if k.startswith(('gw', 'scene')))
        loopback = []
        for observation in window:
            match = re.search(r'(?m)^\s*lo:\s*(.*)$', observation['networkNamespace'])
            if match:
                values = match[1].split()
                loopback.append((observation['monotonicMs'], int(values[0]), int(values[8])))
        row['namespaceLoRxMeanBytesSec'] = ((loopback[-1][1]-loopback[0][1])*1000/(loopback[-1][0]-loopback[0][0])
            if len(loopback) > 1 and loopback[-1][0] > loopback[0][0] else None)
        write_json(cache, row)
        rows.append(row)
    write_json(batch / 'summary.json', rows)
    if rows:
        with (batch / 'summary.csv').open('w') as output:
            writer = csv.DictWriter(output, fieldnames=list(rows[0]))
            writer.writeheader(); writer.writerows(rows)
    return rows


def main():
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--batch', type=pathlib.Path, required=True)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--aggregate', action='store_true')
    parser.add_argument('--limit', type=int)
    parser.add_argument('--case-json', help='Single explicit case object, for capacity smoke')
    parser.add_argument('--approved-3g2s', action='store_true', help='Exactly six approved persistence cases')
    parser.add_argument('--update-report', action='store_true', help='Refresh the report after each completed case')
    args = parser.parse_args()
    args.build, args.batch = args.build.resolve(), args.batch.resolve()
    args.batch.mkdir(parents=True, exist_ok=True)
    if args.aggregate:
        aggregate(args.batch); return
    assert 'CMAKE_BUILD_TYPE:STRING=Release' in (args.build / 'CMakeCache.txt').read_text()
    if not (args.approved_3g2s or args.case_json or args.smoke):
        parser.error('explicit --approved-3g2s, --case-json or --smoke is required; no implicit 480-case run')
    selected = approved_cases() if args.approved_3g2s else [dict(gateways=3, scenes=2,
        connections=12, rps=20, persistence=True, duration=10)]
    if args.case_json:
        selected = [json.loads(args.case_json)]
        assert set(selected[0]) == set(cases()[0])
    if args.limit:
        selected = selected[:args.limit]
    matrixpath = args.batch / 'matrix.json'
    if matrixpath.exists():
        assert json.loads(matrixpath.read_text()) == selected, 'resume matrix mismatch'
    else:
        write_json(matrixpath, selected)
    metadata = dict(uname=platform.uname()._asdict(), cpuCount=os.cpu_count(),
        meminfo=pathlib.Path('/proc/meminfo').read_text(), build=str(args.build), repetitions=1,
        binaries={name: hashlib.sha256((args.build / 'bin' / name).read_bytes()).hexdigest()
                  for name in ('gatewayServer', 'sceneServer', 'gateway_load_client')},
        gitStatus=subprocess.check_output(['git', '-C', str(ROOT), 'status', '--short'], text=True))
    if not (args.batch / 'environment.json').exists():
        write_json(args.batch / 'environment.json', metadata)
    else:
        assert json.loads((args.batch / 'environment.json').read_text())['binaries'] == metadata['binaries'], 'resume binary mismatch'
    source_snapshot = args.batch / 'source-snapshot'
    if not source_snapshot.exists():
        source_snapshot.mkdir()
        for source in [pathlib.Path(__file__), pathlib.Path(__file__).with_name('load_resources.py'),
                       ZONE / 'gatewayServer/tests/load_client.cc', ZONE / 'gatewayServer/tests/loadStats.cc',
                       ZONE / 'gatewayServer/tests/loadStats.h']:
            shutil.copy2(source, source_snapshot / source.name)
        shutil.copy2(args.build / 'CMakeCache.txt', source_snapshot / 'CMakeCache.txt')
    for index, case in enumerate(selected):
        work = args.batch / identity(case)
        if (work / 'result.json').exists() and args.resume:
            continue
        assert not work.exists(), f'refusing to overwrite {work}'
        print(f'START {index+1}/{len(selected)} {work.name}', flush=True)
        result = run_case(args.build, work, case, index)
        for path in list(work.rglob('*.log')) + list(work.glob('*.latency-us.txt')):
            with path.open('rb') as source, gzip.open(str(path)+'.gz', 'wb') as target:
                shutil.copyfileobj(source, target)
            path.unlink()
        print(f'END {work.name} passed={result["passed"]} error={result.get("error", "")}', flush=True)
        aggregate(args.batch)
        if args.update_report:
            subprocess.run([__import__('sys').executable, '-B', str(pathlib.Path(__file__).with_name('benchmark_report.py')),
                            '--batch', str(args.batch)], check=True)
        if args.smoke and not result['passed']:
            raise SystemExit('smoke failed; formal matrix must not start')
    assert exact_case_set(selected, [p.parent.name for p in args.batch.glob('*/result.json')]), 'case set mismatch'
    write_json(args.batch / 'completion.json', dict(completed=len(selected), finishedAt=time.strftime('%Y-%m-%dT%H:%M:%S%z')))
    if args.update_report:
        subprocess.run([__import__('sys').executable, '-B', str(pathlib.Path(__file__).with_name('benchmark_report.py')),
                        '--batch', str(args.batch), '--archive'], check=True)


if __name__ == '__main__':
    main()