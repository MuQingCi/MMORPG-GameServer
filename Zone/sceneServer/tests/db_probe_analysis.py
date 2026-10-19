"""基于采样的 PendingDbTasks 归因，不外推至所有调用。"""
import argparse
import json
import pathlib
from db_maintenance_analysis import load_jsonl


def probe_delta(before,after):
    if len(before)!=2 or len(after)!=2:
        raise ValueError('expected two phases')
    result=[]
    for a,b in zip(before,after):
        if len(a)!=12 or len(b)!=12:
            raise ValueError('expected twelve probe fields')
        d=[y-x for x,y in zip(a,b)]
        if min(d)<0:
            raise ValueError('counter reset')
        n=d[0]
        visited=sum(d[1:6])
        result.append(dict(samples=n,visited=d[1:6],callbacks=d[6],
            historyVisitFraction=d[4]/visited if visited else None,
            wallUsPerCall=d[7]/n/1000 if n else None,
            cpuUsPerCall=d[8]/n/1000 if n else None,
            nonCallbackCpuUsPerCall=(d[8]-d[10])/n/1000 if n else None,
            callbackCpuFraction=d[10]/d[8] if d[8] else None,
            cpuClockFailures=d[11],raw=d))
    return result


def analyze(work):
    meta=json.loads((work/'result.json').read_text())
    start=meta['loadStartMonotonicMs']*1000
    streams=[load_jsonl(p) for p in sorted(work.glob('*queues.jsonl'))]
    if not streams:
        raise ValueError('missing probe streams')
    output=[]
    for sec in range(0,meta['case']['duration'],30):
        before=[[0]*12 for _ in range(2)]
        after=[[0]*12 for _ in range(2)]
        offsets=[]
        for rows in streams:
            full=[r for r in rows if r.get('work')]
            boundaries=[]
            for edge in (sec,sec+30):
                deadline=start+edge*1000000
                valid=[r for r in full if r['monotonicUs']<=deadline]
                if not valid or deadline-valid[-1]['monotonicUs']>2000000:
                    raise ValueError('missing/stale boundary')
                boundaries.append(valid[-1])
            a,b=boundaries
            offsets.append([a['monotonicUs']-start-sec*1000000,b['monotonicUs']-start-(sec+30)*1000000])
            for target,row in ((before,a),(after,b)):
                for w in row['work'][:4]:
                    for p in range(2):
                        for k in range(12):target[p][k]+=w['dbProbe'][p][k]
        output.append(dict(startSec=sec,endSec=sec+30,phases=probe_delta(before,after),boundaryOffsetsUs=offsets))
    return output


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work',type=pathlib.Path)
    args=parser.parse_args()
    (args.work/'probe-analysis.json').write_text(json.dumps(analyze(args.work),indent=2)+'\n')