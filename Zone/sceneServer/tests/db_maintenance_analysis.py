#!/usr/bin/env python3
"""仅分析诊断的 30 秒窗口，不执行负载，不推断因果。"""
import argparse
import gzip
import json
import pathlib

PHASES=('load_drain_reload','flush','aoi_recovery','persistence_retry',
        'aoi_tick','aoi_recovery_only','persistence_tick','db_tick','db_controls','diagnostic_scan')


def summarize(streams,start_us,duration,workers=4):
    if duration<=0 or duration%30 or not streams:
        raise ValueError('expected diagnostic streams and positive 30s windows')
    prepared=[]
    for rows in streams:
        ordered=sorted(rows,key=lambda r:r['monotonicUs'])
        full=[r for r in ordered if r.get('work')]
        if not full:
            raise ValueError('missing full diagnostic snapshots')
        prepared.append((ordered,full))
    result=dict(phaseNames=PHASES,windows=[])
    for second in range(0,duration,30):
        begin=start_us+second*1000000
        end=begin+30000000
        phase=[0]*10
        tasks0=[0]*6
        tasks1=[0]*6
        scans=[0]*2
        rejected=[0]*6
        net_delta=[0]*10
        calls=handlers=0
        depth=net=0
        boundary_offsets=[]
        sampled_buffer=sampled_max_buffer=0
        for rows,full in prepared:
            before=[r for r in full if r['monotonicUs']<=begin]
            after=[r for r in full if r['monotonicUs']<=end]
            if not before or not after:
                raise ValueError('missing boundary snapshot')
            a,b=before[-1],after[-1]
            if len(a['work'])!=len(b['work']):
                raise ValueError('worker cardinality changed')
            if begin-a['monotonicUs']>2000000 or end-b['monotonicUs']>2000000:
                raise ValueError('boundary more than 2s stale')
            boundary_offsets.append([a['monotonicUs']-begin,b['monotonicUs']-end])
            for x,y in zip(a['work'][:workers],b['work'][:workers]):
                def delta(u,v):
                    value=v-u
                    if value<0:
                        raise ValueError('diagnostic counter reset')
                    return value
                for index in range(10):
                    phase[index]+=delta(x['phases'][index]['sumUs'],y['phases'][index]['sumUs'])
                calls+=delta(x['phases'][7]['count'],y['phases'][7]['count'])
                for index in range(6):
                    tasks0[index]+=x['dbTasks'][index]
                    tasks1[index]+=y['dbTasks'][index]
                    rejected[index]+=delta(x['classes'][index]['rejected'],y['classes'][index]['rejected'])
                    handlers+=delta(x['classes'][index]['handled']['sumUs'],y['classes'][index]['handled']['sumUs'])
                for index in range(2):
                    scans[index]+=delta(x['dbScanned'][index],y['dbScanned'][index])
                for index in (0,1,2,3,7,8,9):
                    net_delta[index]+=delta(x['netPending'][index],y['netPending'][index])
            for row in rows:
                if begin<=row['monotonicUs']<=end:
                    depth=max(depth,max(row['depths'][:workers],default=0))
                    net=max(net,row['netQueue'])
                    for event in row.get('events',[]):
                        # 完整采集会排空事件；应使用事件的实际发生时间。
                        if begin<=event['beginUs']<=end:
                            if event['kind']=='net_send_buffer_bytes':
                                sampled_buffer=max(sampled_buffer,event['count'])
                            if event['kind']=='net_send_buffer_max_bytes':
                                sampled_max_buffer=max(sampled_max_buffer,event['count'])
        scans_us=phase[7]+phase[8]
        measured=sum(phase[:4])+handlers
        result['windows'].append(dict(startSec=second,endSec=second+30,
            phaseUs=phase,handlerUs=handlers,maintenanceCalls=calls,dbScanned=scans,
            dbTasksStart=tasks0,dbTasksEnd=tasks1,twoScanMeanUs=scans_us/calls if calls else None,
            scanShareOfMeasuredWork=scans_us/measured if measured else None,
            workerQueueSampledPeak=depth,netQueueSampledPeak=net,workerRejected=rejected,
            netPendingCumulativeDelta=net_delta,boundaryOffsetsUs=boundary_offsets,
            sendBufferTotalSampledPeakBytes=sampled_buffer,sendBufferMaxSampledPeakBytes=sampled_max_buffer))
    return result


def load_jsonl(path):
    opener=gzip.open if path.suffix=='.gz' else open
    with opener(path,'rt') as source:
        return [json.loads(line) for line in source]


def analyze(work):
    result=json.loads((work/'result.json').read_text())
    paths=sorted(work.glob('*.queues.jsonl')) or sorted(work.glob('*.queues.jsonl.gz'))
    start=result['loadStartMonotonicMs']*1000
    summary=summarize([load_jsonl(p) for p in paths],start,result['case']['duration'])
    metrics=[load_jsonl(p) for p in sorted(work.glob('metrics-gw*.jsonl'))]
    def at(second,key):
        return sum(next(row[key] for row in rows if row['elapsedMs']==second*1000) for rows in metrics)
    for row in summary['windows']:
        a,b=row['startSec'],row['endSec']
        row['successfulRps']=(at(b,'client_window_succeeded')-at(a,'client_window_succeeded'))/30
        row['clientInFlightStart']=at(a,'client_in_flight')
        row['clientInFlightEnd']=at(b,'client_in_flight')
        row['clientScheduleMaxUsEnd']=at(b,'client_schedule_delay_max_us')
    summary['caveats']=[
        'One diagnostic run only; not A/B and no unique-root-cause claim.',
        'Full boundaries precede the requested boundaries; offsets recorded.',
        'Detailed phases overlap their parent phases; shares use phases 0..3 plus handlers only.',
        'Phase9 isolates state-scan wall time, not all diagnostic overhead.',
        'Wall time includes preemption; queue/buffer peaks are sampled, not exhaustive.',
        'Net pending fields are approximately sampled once per second; cross-field reads are not atomic.',
        'Counter stream success at t=0 can include boundary sampling skew; original capacity summary is unchanged.'
    ]
    return summary


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work',type=pathlib.Path)
    args=parser.parse_args()
    value=analyze(args.work.resolve())
    (args.work/'maintenance-analysis.json').write_text(json.dumps(value,indent=2)+'\n')