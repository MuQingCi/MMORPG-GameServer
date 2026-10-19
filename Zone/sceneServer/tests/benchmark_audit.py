#!/usr/bin/env python3
"""只读证据审计：不依赖聚合缓存，重新计算全局分位数。

退出码 1 表示证据不一致，并非仅仅性能用例失败。
批次运行期间，只审计完整的结果/缓存对。
"""
import argparse
import gzip
import hashlib
import json
import math
import pathlib
import re
from benchmark_matrix import exact_case_set


def audit_case(work):
    issues = []
    result = json.loads((work / 'result.json').read_text())
    case = result['case']
    clients = [json.loads(p.read_text()) for p in work.glob('load-gw*.json')]
    if len(clients) != case['gateways']:
        issues.append('missing client summaries')
    if sum(c['connections'] for c in clients) != case['connections']:
        issues.append('connection total differs from case')
    for index, client in enumerate(clients):
        if client['load_attempted'] != client['completed'] + client['unresolved']:
            issues.append(f'client {index}: request conservation failed')
    samples = []
    for path in work.glob('*.latency-us.txt*'):
        with (gzip.open(path, 'rt') if path.suffix == '.gz' else path.open()) as source:
            samples.extend(int(line) for line in source)
    samples.sort()
    if len(samples) != sum(c['business_succeeded'] for c in clients):
        issues.append('sample count differs from successful requests')
    cached = json.loads((work / 'case-summary.json').read_text())
    if cached['latencySamples'] != len(samples):
        issues.append('cached latencySamples differs from raw samples')
    for key, fraction in (('p50Us', .5), ('p90Us', .9), ('p99Us', .99)):
        value = samples[math.ceil(len(samples)*fraction)-1] if samples else None
        if cached[key] != value:
            issues.append(key + ' differs from raw samples')
    limit = 100000 if case['connections']*case['rps'] > 10000 else 10000
    for name, expected in (('gw', case['gateways']), ('scene', case['scenes'])):
        paths = list(work.glob(name + '*.yaml'))
        if len(paths) != expected:
            issues.append(f'{name}: missing generated configs')
        for path in paths:
            text = path.read_text()
            if re.findall(r'(?m)^\s*maxFramesPerSec:\s*(\d+)', text) != [str(limit)]:
                issues.append(path.name + ': invalid or duplicate frame limit')
            if name == 'scene' and len(re.findall(r'(?m)^    net:', text)) != 1:
                issues.append(path.name + ': invalid or duplicate net section')
    if case['persistence'] and result['passed']:
        proof = json.loads((work / 'persistence-verification.json').read_text())
        if not proof.get('finalSnapshotsMatched'):
            issues.append('missing final DB snapshot proof')
        if not proof.get('reloadedPlayers'):
            issues.append('missing reload proof')
        if (result.get('onlineSaveEvidence') or {}).get('advancedPlayers') != case['connections']:
            issues.append('missing online save proof')
        if '<redacted>' not in (work / 'zone.yaml').read_text():
            issues.append('zone password not redacted')
    return dict(id=work.name, performancePassed=result['passed'], samples=len(samples), issues=issues)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--batch', type=pathlib.Path, required=True)
    parser.add_argument('--require-complete', action='store_true')
    parser.add_argument('--archived', action='store_true', help='Verify archived SHA256SUMS instead of removed build binaries')
    args = parser.parse_args()
    batch = args.batch.resolve()
    issues, rows = [], []
    meta = json.loads((batch / 'environment.json').read_text())
    for name, expected in ({} if args.archived else meta['binaries']).items():
        path = pathlib.Path(meta['build']) / 'bin' / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            issues.append('current binary missing or hash mismatch: ' + name)
    if args.archived:
        sums = batch / 'SHA256SUMS'
        if not sums.is_file():
            issues.append('missing archive checksums')
        else:
            for line in sums.read_text().splitlines():
                expected, relative = line.split('  ', 1)
                path = batch / relative
                if path.is_symlink() or not path.resolve().is_relative_to(batch) or not path.is_file():
                    issues.append('unsafe or missing archive file: ' + relative)
                elif hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                    issues.append('archive checksum mismatch: ' + relative)
    for result in sorted(batch.glob('*/result.json')):
        work = result.parent
        if not (work / 'case-summary.json').exists():
            continue  # producer may still be compressing/aggregating this case
        try:
            rows.append(audit_case(work))
        except (OSError, ValueError, KeyError) as error:
            issues.append(work.name + ': ' + str(error))
    if args.require_complete:
        selected = json.loads((batch / 'matrix.json').read_text())
        if not (batch / 'completion.json').exists() or not exact_case_set(selected, [r['id'] for r in rows]):
            issues.append('approved case set incomplete or unexpected cases present')
    report = dict(batch=str(batch), audited=len(rows), valid=not issues and not any(r['issues'] for r in rows),
                  issues=issues, cases=rows)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    raise SystemExit(0 if report['valid'] else 1)


if __name__ == '__main__':
    main()