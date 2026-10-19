#!/usr/bin/env python3
"""离线按 100ms 聚合；区间表示采样时非空，不能证明持续积压。"""
import argparse
import collections
import json
import pathlib
import bisect


def phase_samples(rows, start_us, seconds, offset_us, tolerance_us=6000):
    times=[r['monotonicUs'] for r in rows]
    values=[]
    for second in range(seconds):
        target=start_us+second*1000000+offset_us
        index=bisect.bisect_left(times,target)
        candidates=[i for i in (index-1,index) if 0<=i<len(rows)]
        if not candidates:
            values.append(None)
            continue
        closest=min(candidates,key=lambda i:abs(times[i]-target))
        values.append(sum(rows[closest]['depths'])+rows[closest]['netQueue']
                      if abs(times[closest]-target)<=tolerance_us else None)
    return dict(offsetUs=offset_us,matched=sum(v is not None for v in values),
                missing=values.count(None),nonempty=sum(v is not None and v>0 for v in values),values=values)


def wait_windows(rows):
    snapshots=[r for r in rows if r.get('waits')]
    result=[]
    for before,after in zip(snapshots,snapshots[1:]):
        previous={(w['worker'],w['category']):w for w in before['waits']}
        for w in after['waits']:
            old=previous.get((w['worker'],w['category']))
            if old is None:
                continue
            count=w['count']-old['count'];total=w['sumUs']-old['sumUs']
            bins=[a-b for a,b in zip(w['bins'],old['bins'])]
            valid=count>=0 and total>=0 and all(b>=0 for b in bins) and sum(bins)==count
            result.append(dict(startUs=before['monotonicUs'],endUs=after['monotonicUs'],
                               worker=w['worker'],category=w['category'],valid=valid,
                               count=count,bins=bins,meanUs=total/count if valid and count else None))
    return result


def task_pairs(events):
    tasks=collections.defaultdict(lambda:collections.defaultdict(list))
    for event in events:
        if event['kind'].startswith('save_'):
            tasks[event['count']][event['kind']].append(event)
    records=[]
    for task,stages in tasks.items():
        required=('save_registered','save_db_accepted','save_db_execute','save_completed')
        missing=[stage for stage in required if not stages.get(stage)]
        record=dict(taskId=task,missing=missing,duplicateStages=[k for k,v in stages.items() if len(v)>1])
        if not missing:
            reg=stages['save_registered'][0];accepted=stages['save_db_accepted'][0]
            execution=stages['save_db_execute'][0];completed=stages['save_completed'][0]
            record.update(dbQueueUs=execution['beginUs']-accepted['beginUs'],
                          executeUs=execution['endUs']-execution['beginUs'],
                          returnAndApplyUs=completed['endUs']-execution['endUs'],
                          totalUs=completed['endUs']-reg['beginUs'])
        records.append(record)
    return records


def summarize(rows):
    rows = sorted(rows, key=lambda row: row["monotonicUs"])
    if not rows:
        raise ValueError("empty diagnostic stream")
    origin = rows[0]["monotonicUs"]
    groups = collections.defaultdict(list)
    gaps = []
    previous = None
    streak_begin = None
    longest = 0
    empty = 0
    maximum = 0
    sampling_gaps = 0
    events = collections.Counter()
    intervals = []
    for row in rows:
        now = row["monotonicUs"]
        if previous is not None:
            gaps.append(now - previous)
            if now-previous>30000:
                sampling_gaps+=1
                streak_begin=None
        previous = now
        depth = sum(row["depths"]) + row["netQueue"]
        maximum = max(maximum, depth)
        groups[(now - origin) // 100000].append((now, depth))
        if depth:
            if streak_begin is None:
                streak_begin = now
            longest = max(longest, now - streak_begin)
        else:
            empty += 1
            streak_begin = None
        for event in row["events"]:
            events[event["kind"]] += 1
            intervals.append(event)
    windows = []
    for index, samples in sorted(groups.items()):
        values = [value for _, value in samples]
        windows.append(dict(startUs=origin + index * 100000, samples=len(values),
                            min=min(values), max=max(values), avg=sum(values) / len(values),
                            nonemptyRatio=sum(value > 0 for value in values) / len(values)))
    return dict(samples=len(rows), emptySamples=empty, maxDepth=maximum,
                longestSampledNonemptyUs=longest, maxSampleGapUs=max(gaps, default=0),
                events=dict(events), eventIntervals=intervals, windows=windows,
                waits=next((r['waits'] for r in reversed(rows) if r.get('waits')),[]),
                droppedEvents=max(r.get('droppedEvents',0) for r in rows),
                writeFailed=any(r.get('writeFailed',False) for r in rows),
                samplingGaps=sampling_gaps,waitWindows=wait_windows(rows),tasks=task_pairs(intervals),
                caveats=["Queue sizes are approximate; a zero/nonzero sample is not continuous observation.",
                         "Wait histograms exclude messages not dequeued; bin upper bounds in us: 100,500,1000,2000,5000,10000,20000,50000,100000,infinity.",
                         "Event count is tick sequence / dirty players / task id, according to event kind."])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=pathlib.Path)
    args = parser.parse_args()
    result = summarize([json.loads(line) for line in args.path.read_text().splitlines()])
    args.path.with_suffix(".summary.json").write_text(json.dumps(result, indent=2) + "\n")