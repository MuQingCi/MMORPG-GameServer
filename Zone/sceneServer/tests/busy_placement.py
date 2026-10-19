#!/usr/bin/env python3
"""冻结布置的预检，绝不启动测量业务流量。"""
import argparse
import hashlib
import json
import os
import pathlib

import benchmark_matrix as matrix
from busy_timeline import actor_worker


def canonical(players):
    return sorted([dict(playerId=p['playerId'], gateway=p['gateway'],
                        sceneId=p['sceneId'], worker=p.get('worker', actor_worker(p['playerId'])),
                        x=p['x'], y=p.get('y', 6), saveVersion=p.get('saveVersion', 7))
                   for p in players], key=lambda p: p['playerId'])


def validate(expected, actual):
    a, b = canonical(expected), canonical(actual)
    def fingerprint(rows):
        return hashlib.sha256(json.dumps(rows, sort_keys=True).encode()).hexdigest()
    unique = len({p['playerId'] for p in a}) == len(a) and len({p['playerId'] for p in b}) == len(b)
    return dict(matched=unique and a == b, unique=unique,
                expectedFingerprint=fingerprint(a), actualFingerprint=fingerprint(b),
                differences=[dict(expected=x, actual=y) for x, y in zip(a, b) if x != y],
                expectedCount=len(a), actualCount=len(b))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--batch', type=pathlib.Path, required=True)
    parser.add_argument('--manifest', type=pathlib.Path, required=True)
    args = parser.parse_args()
    build, batch = args.build.resolve(), args.batch.resolve()
    batch.mkdir(parents=True, exist_ok=False)
    expected = json.loads(args.manifest.read_text())
    assert len(expected) == 256 and validate(expected, expected)['matched']
    assert 'CMAKE_BUILD_TYPE:STRING=Release' in (build/'CMakeCache.txt').read_text()
    for key in ('SCENE_QUEUE_DIAGNOSTICS', 'SCENE_EXPERIMENT_ACTIVE_DB_INDEX', 'AOI_VERIFY_TOTAL'):
        os.environ.pop(key, None)
    matrix.write_json(batch/'frozen-players.json', canonical(expected))
    matrix.write_json(batch/'environment.json', dict(binaries={name: hashlib.sha256((build/'bin'/name).read_bytes()).hexdigest()
        for name in ('sceneServer', 'gatewayServer', 'gateway_load_client')}, businessTraffic=False))
    rows = []
    for attempt in range(1, 4):
        case = dict(gateways=3, scenes=2, connections=256, rps=20, persistence=True, duration=120)
        result = matrix.run_case(build, batch/f'prepare-{attempt}', case, 0,
                                 frozen_players=expected, preparation_only=True)
        rows.append(dict(attempt=attempt, passed=result['passed'], error=result.get('error')))
        matrix.write_json(batch/'progress.json', rows)
    matrix.write_json(batch/'completion.json', dict(attempts=rows,
        readyForNextStage=all(r['passed'] for r in rows), businessTraffic=False,
        reason='preparation-validated' if all(r['passed'] for r in rows) else 'placement-gate-stop'))


if __name__ == '__main__':
    main()