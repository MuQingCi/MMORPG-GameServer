#!/usr/bin/env python3
"""只读归约 AOI 原始诊断，绝不写入用例证据缓存。"""
import argparse
import gzip
import json
import math
import pathlib

STAGES = ('query', 'buildDiff', 'serialize', 'clientAdmission', 'commit', 'observerPass', 'remotePublish')
COUNTERS = ('cells', 'candidates', 'visible', 'observers', 'pushEntities', 'bytes', 'accepted', 'rejected')


def counter_delta(begin, end):
    if end < begin:
        raise ValueError('cumulative counter reset')
    return end - begin


def per_unit(value, units):
    return value / units if units else None


def quantile_range(bins, quantile):
    if len(bins) != 10 or not 0 < quantile <= 1 or any(x < 0 for x in bins):
        raise ValueError('invalid histogram')
    count = sum(bins)
    if not count:
        return None
    rank = math.ceil(count * quantile)
    bounds = (100, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, None)
    seen = 0
    for index, amount in enumerate(bins):
        seen += amount
        if seen >= rank:
            return (0 if index == 0 else bounds[index-1] + 1, bounds[index])


def select_window(rows, begin, end):
    selected = sorted((r for r in rows if begin <= r['monotonicUs'] <= end and
                       r.get('fullSnapshot', True)), key=lambda r: r['monotonicUs'])
    if len(selected) < 2 or selected[0]['monotonicUs'] == selected[-1]['monotonicUs']:
        raise ValueError('insufficient full samples inside measurement window')
    return selected[0], selected[-1]


def reduce_window(first, last):
    if len(first['aoi']) != len(last['aoi']):
        raise ValueError('worker count changed')
    result = dict(beginUs=first['monotonicUs'], endUs=last['monotonicUs'], workers=[])
    for a, b in zip(first['aoi'], last['aoi']):
        counters = {name: counter_delta(x, y) for name, x, y in
                    zip(COUNTERS, a['counters'], b['counters'])}
        stages = {}
        for name, x, y in zip(STAGES, a['stages'], b['stages']):
            bins = [counter_delta(i, j) for i, j in zip(x['bins'], y['bins'])]
            elapsed = counter_delta(x['sumUs'], y['sumUs'])
            stages[name] = dict(sumUs=elapsed, count=sum(bins), bins=bins,
                                p99RangeUs=quantile_range(bins, .99),
                                usPerObserver=per_unit(elapsed, counters['observers']),
                                usPerVisibleEntity=per_unit(elapsed, counters['visible']))
        result['workers'].append(dict(counters=counters, stages=stages,
            invalidTimes=counter_delta(a['invalidTimes'], b['invalidTimes'])))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('diagnostics', type=pathlib.Path)
    parser.add_argument('--begin-us', type=int, required=True)
    parser.add_argument('--end-us', type=int, required=True)
    args = parser.parse_args()
    opener = gzip.open if args.diagnostics.suffix == '.gz' else open
    with opener(args.diagnostics, 'rt') as stream:
        rows = [json.loads(line) for line in stream if line.strip()]
    first, last = select_window(rows, args.begin_us, args.end_us)
    print(json.dumps(reduce_window(first, last), indent=2))


if __name__ == '__main__':
    main()