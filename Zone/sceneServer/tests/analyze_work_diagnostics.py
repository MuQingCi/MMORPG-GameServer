#!/usr/bin/env python3
"""汇总 START..cutoff 的累计增量，不声称直方图能给出精确分位数。"""
import argparse
import json
import pathlib

CLASSES=('business','db_result','service','timer','lifecycle','other')
PHASES=('load_drain_reload','flush','aoi_recovery','persistence_retry')


def summarize(rows,start_us,duration):
    rows=sorted(rows,key=lambda r:r['monotonicUs'])
    full=[r for r in rows if r.get('work')]
    before=[r for r in full if r['monotonicUs']<=start_us]
    after=[r for r in full if r['monotonicUs']<=start_us+duration*1000000]
    if not before or not after:raise ValueError('missing boundary diagnostic snapshots')
    first,last=before[-1],after[-1]
    result=dict(startOffsetUs=first['monotonicUs']-start_us,endOffsetUs=last['monotonicUs']-start_us,
                seconds=(last['monotonicUs']-first['monotonicUs'])/1e6,workers=[],
                caveats=['Cumulative snapshots are not globally atomic','Boundary snapshots may precede START/cutoff',
                         'Queue depths approximate; waits exclude not-dequeued messages; diagnostic/profiling overhead included'])
    for wid,(a,b) in enumerate(zip(first['work'][:4],last['work'][:4])):
        item=dict(worker=wid,classes={},phases={})
        for name,x,y in zip(CLASSES,a['classes'],b['classes']):
            value={k:y[k]-x[k] for k in ('attempted','accepted','rejected')}
            value['firstRejectedOffsetUs']=y['firstRejectedUs']-start_us if y['firstRejectedUs'] else None
            value['handled']=y['handled']['count']-x['handled']['count']
            value['handlerUs']=y['handled']['sumUs']-x['handled']['sumUs']
            if any(value[k]<0 for k in ('attempted','accepted','rejected','handled','handlerUs')):
                raise ValueError('diagnostic counter reset')
            if value['attempted'] != value['accepted']+value['rejected']:
                raise ValueError('diagnostic admission conservation failed')
            value['meanHandlerUs']=value['handlerUs']/value['handled'] if value['handled'] else None
            item['classes'][name]=value
        for name,x,y in zip(PHASES,a['phases'],b['phases']):
            item['phases'][name]={k:y[k]-x[k] for k in ('count','sumUs')}
        window=[r['depths'][wid] for r in rows if start_us<=r['monotonicUs']<=start_us+duration*1000000]
        item['sampledDepthPeak']=max(window,default=None)
        result['workers'].append(item)
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('work',type=pathlib.Path)
    args=parser.parse_args();r=json.loads((args.work/'result.json').read_text())
    summaries={p.name:summarize([json.loads(line) for line in p.read_text().splitlines()],
                                r['loadStartMonotonicMs']*1000,r['case']['duration'])
               for p in args.work.glob('*.queues.jsonl')}
    if not summaries:raise SystemExit('missing queues.jsonl evidence')
    (args.work/'work-diagnostic-summary.json').write_text(json.dumps(summaries,indent=2)+'\n')