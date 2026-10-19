#!/usr/bin/env python3
"""保存每工作线程观测与精确拒绝关联，不进行因果分类。"""
import argparse
import collections
import gzip
import json
import pathlib
import re

from busy_analysis import classify, correlate
from db_maintenance_analysis import analyze as maintenance
from db_probe_analysis import analyze as probes, probe_delta


def read_rows(path):
    with path.open() as stream: return [json.loads(line) for line in stream]


def actor_worker(player):
    mask=(1<<64)-1
    x=(player+0x9E3779B97F4A7C15)&mask
    x=((x^(x>>30))*0xBF58476D1CE4E5B9)&mask
    x=((x^(x>>27))*0x94D049BB133111EB)&mask
    return (x^(x>>31))%4


def analyze(work):
    meta=json.loads((work/'result.json').read_text())
    start=meta['loadStartMonotonicMs']*1000
    end=start+meta['case']['duration']*1000000
    scenes=[]
    all_events=[]
    for path in sorted(work.glob('*.busy.jsonl')):
        rows=read_rows(path)
        trace=classify(rows,start,end)
        trace['source']=path.name
        all_events.extend(e for r in rows for e in r['busy']['events'])
        trace['workerTids']=rows[-1]['busy']['workerTids']
        scenes.append(trace)
    routes=[]
    for path in work.glob('logs/*/gw*.log.gz'):
        gateway=int(path.name[2])
        with gzip.open(path,'rt') as stream:
            for line in stream:
                match=re.search(r'relay b->c: session=\s*(\d+).*?seq=\s*(\d+)\s*-> requestId=\s*(\d+)',line)
                if match:
                    routes.append(dict(gatewayId=gateway,clientSession=int(match[1]),seq=int(match[2]),requestId=int(match[3])))
    clients=[]
    lost=False
    for path in work.glob('client*.stdout.log.gz'):
        with gzip.open(path,'rt') as stream:
            for line in stream:
                if 'BUSY_EVIDENCE_DROPPED' in line: lost=True
                match=re.search(r'BUSY_EVIDENCE player=(\d+) request=(\d+) receivedUs=(\d+) measured=(\d+)',line)
                if match:
                    clients.append(dict(playerId=int(match[1]),requestId=int(match[2]),receivedUs=int(match[3]),measured=bool(int(match[4]))))
    correlation=correlate(all_events,routes,clients)
    correlation['clientDropped']=lost
    correlation['complete']=correlation['complete'] and not lost and all(s['complete'] for s in scenes)
    correlation['measuredClientCount']=sum(c['measured'] for c in clients)
    (work/'correlation.json').write_text(json.dumps(correlation,indent=2)+'\n')
    players=json.loads((work/'players.json').read_text())
    distribution=collections.Counter((p['sceneId'],actor_worker(p['playerId'])) for p in players)
    output=dict(scenes=scenes,playersPerWorker=[dict(scene=s,worker=w,players=n) for (s,w),n in sorted(distribution.items())],workers=[])
    resources=read_rows(work/'resources.jsonl')
    for path in sorted(work.glob('*.queues.jsonl')):
        scene=int(re.search(r'scene(\d+)',path.name)[1])
        records=read_rows(path)
        full=[r for r in records if r.get('work')]
        tids=next(s['workerTids'] for s in scenes if s['source'].startswith(f'scene{scene}.'))
        for worker,tid in enumerate(tids):
            windows=[]
            for sec in range(0,120,30):
                edges=[]
                for offset in (sec,sec+30):
                    valid=[r for r in full if r['monotonicUs']<=start+offset*1000000]
                    if not valid: raise ValueError('missing timeline boundary')
                    edges.append(valid[-1])
                a,b=edges
                x,y=a['work'][worker],b['work'][worker]
                accepted=[v['accepted']-u['accepted'] for u,v in zip(x['classes'],y['classes'])]
                handled=[v['handled']['count']-u['handled']['count'] for u,v in zip(x['classes'],y['classes'])]
                rejected=[v['rejected']-u['rejected'] for u,v in zip(x['classes'],y['classes'])]
                tasks=[]
                for r in resources:
                    if start+sec*1000000<=r['monotonicMs']*1000<=start+(sec+30)*1000000:
                        for p in r['processes']:
                            if p['role']==f'scene{scene}':
                                tasks.extend((r['monotonicMs'],t,r['clockTicks']) for t in p['tasks'] if t['tid']==tid)
                cpu=wait=None
                if len(tasks)>1:
                    ta,tb=tasks[0],tasks[-1];dt=(tb[0]-ta[0])/1000
                    if dt>0:
                        cpu=(tb[1]['cpuTicks']-ta[1]['cpuTicks'])/tb[2]/dt
                        if 'runqueueWaitNs' in ta[1] and 'runqueueWaitNs' in tb[1]:wait=(tb[1]['runqueueWaitNs']-ta[1]['runqueueWaitNs'])/1e9/dt
                windows.append(dict(startSec=sec,endSec=sec+30,boundaryOffsetsUs=[a['monotonicUs']-start-sec*1000000,b['monotonicUs']-start-(sec+30)*1000000],
                    historyStart=x['dbTasks'][4],historyEnd=y['dbTasks'][4],accepted=accepted,handled=handled,rejected=rejected,
                    sampledDepthPeak=max((r['depths'][worker] for r in records if start+sec*1000000<=r['monotonicUs']<start+(sec+30)*1000000),default=0),
                    probe=probe_delta(x['dbProbe'],y['dbProbe']),cpuCores=cpu,runqueueWaitSecondsPerSecond=wait))
            output['workers'].append(dict(scene=scene,worker=worker,tid=tid,windows=windows))
    (work/'timeline.json').write_text(json.dumps(output,indent=2)+'\n')
    if output['workers']:
        (work/'maintenance-analysis.json').write_text(json.dumps(maintenance(work),indent=2)+'\n')
        (work/'probe-analysis.json').write_text(json.dumps(probes(work),indent=2)+'\n')
    return output,correlation


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('work',type=pathlib.Path)
    analyze(parser.parse_args().work.resolve())