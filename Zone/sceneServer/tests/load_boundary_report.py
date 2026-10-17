#!/usr/bin/env python3
"""Offline load/recovery report; never controls execution or alters evidence."""
import argparse
import json
import pathlib
import re


def analyze_samples(rows, duration):
    peaks, windows = {}, []
    recovery = None
    for row in rows:
        for key, value in row.items():
            if re.fullmatch(r'scene\d+_worker\d+_queue', key):
                peaks[key] = max(peaks.get(key, 0), value)
        # Missing/invalid measurements cannot prove recovery.
        required = ['client_in_flight', 'gateway_pendingRoutes']
        required += [f'scene{s}_worker{w}_queue' for s in (0, 1) for w in range(4)]
        required += [f'scene{s}_netQueue' for s in (0, 1)]
        if (recovery is None and row['elapsedMs'] >= duration*1000
                and row.get('metricsValid') is True
                and row.get('gateway_backendReady', 0) >= 2
                and all(row.get(k) == 0 for k in required)):
            recovery = row['elapsedMs'] - duration*1000
    for before, after in zip(rows, rows[1:]):
        if after['elapsedMs'] > duration*1000:
            continue
        seconds = (after['readMonotonicUs']-before['readMonotonicUs'])/1e6
        if seconds <= 0:
            continue
        windows.append(dict(elapsedMs=after['elapsedMs'], actualIntervalSec=seconds,
            sendRps=(after.get('client_sent', 0)-before.get('client_sent', 0))/seconds,
            successfulRps=(after.get('client_business_succeeded', 0)
                           -before.get('client_business_succeeded', 0))/seconds))
    return dict(peakWorkerDepths=peaks, sampledRecoveryMs=recovery, loadWindows=windows,
                recoveryResolution='nominal 1s; zero queues/inflight/routes and backendReady>=2 registered scene identities; not a channel/probe guarantee',
                invalidSamples=sum(r.get('metricsValid') is not True for r in rows))


def resource_peaks(samples):
    peaks, previous = {}, {}
    for sample in samples:
        for row in sample['processes']:
            key = (row['pid'], row['startTicks'])
            peak = peaks.setdefault(row['name'], dict(peakCpuPercent=0, peakRssBytes=0))
            peak['peakRssBytes'] = max(peak['peakRssBytes'], row['rssBytes'])
            if key in previous:
                before, ticks = previous[key]
                seconds = (sample['monotonicMs']-before)/1000
                if seconds > 0:
                    cpu = 100*(row['cpuTicks']-ticks)/sample['clockTicks']/seconds
                    peak['peakCpuPercent'] = max(peak['peakCpuPercent'], cpu)
            previous[key] = (sample['monotonicMs'], row['cpuTicks'])
    return peaks


def read_jsonl(path):
    rows = []
    if path.exists():
        for line in path.read_text().splitlines():
            try:
                rows.append(json.loads(line))
            except ValueError:
                continue  # A partial last write must not destroy previous evidence.
    return rows


def report_batch(path):
    runs = []
    for result in sorted(path.glob('*-*/result.json')):
        row = json.loads(result.read_text())
        artifact = pathlib.Path(row['evidence'])
        duration = next((g['raw']['duration_sec'] for g in row['gateways']), 60)
        row['samples'] = {str(g): analyze_samples(
            read_jsonl(artifact/'run'/f'metrics-gw{g}.jsonl'), duration) for g in (1, 2)}
        row['resourcePeaks'] = resource_peaks(read_jsonl(artifact/'resources.jsonl'))
        verification = artifact/'run'/'persistence-verification.json'
        row['persistenceVerification'] = json.loads(verification.read_text()) if verification.exists() else None
        row['recoveryServiceSnapshots'] = read_jsonl(artifact/'run'/'recovery.jsonl')
        # Closed-loop underdelivery may be server response backpressure OR generator
        # saturation. Do not infer either cause solely from configured/actual RPS.
        row['capacityAttribution'] = 'same-host closed-loop; generator limitation not excluded'
        runs.append(row)
    return dict(evidence=str(path), runs=runs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('batch', type=pathlib.Path)
    args = parser.parse_args()
    report = report_batch(args.batch.resolve())
    output = args.batch/'boundary-report.json'
    output.write_text(json.dumps(report, indent=2)+'\n')
    print(output.resolve())