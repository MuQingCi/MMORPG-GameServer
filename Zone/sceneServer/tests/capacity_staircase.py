#!/usr/bin/env python3
"""仅运行已批准的顺序容量负载，不运行 smoke/准入/回归。"""
import argparse
import collections
import gzip
import hashlib
import json
import os
import pathlib
import signal
import time
import benchmark_matrix as matrix


def measure(build, batch, connections, duration):
    work=batch/f'c{connections}-d{duration}'
    assert not work.exists(), f'refusing overwrite {work}'
    case=dict(gateways=3,scenes=2,connections=connections,rps=20,persistence=True,duration=duration)
    print('START',work.name,flush=True)
    result=matrix.run_case(build,work,case,0,capacity=True)
    clients=[json.loads(p.read_text()) for p in sorted(work.glob('load-gw*.json'))]
    if len(clients)!=3:
        row=dict(connections=connections,duration=duration,valid=False,stable=False,error=result.get('error','missing clients'))
        matrix.write_json(work/'capacity-summary.json',row)
        return row
    samples=[[json.loads(l) for l in p.read_text().splitlines()] for p in sorted(work.glob('metrics-gw*.jsonl'))]
    def at(sec,key):
        return sum(next(r[key] for r in rows if r['elapsedMs']==sec*1000) for rows in samples)
    success=at(duration,'client_window_succeeded')
    sent=sum(c['sent'] for c in clients)
    succeeded=sum(c['business_succeeded'] for c in clients)
    unresolved=sum(c['unresolved'] for c in clients)
    completed=sum(c['completed'] for c in clients)
    errors=collections.Counter()
    for c in clients:errors.update(c['business_errors'])
    latencies=[]
    for p in work.glob('*.latency-us.txt'):
        latencies.extend(map(int,p.read_text().splitlines()))
    pct=matrix.percentiles(latencies)
    ordered=sorted(latencies)
    p95=ordered[min(len(ordered)-1,int(len(ordered)*.95))] if ordered else None
    last=(at(duration,'client_window_succeeded')-at(duration-30,'client_window_succeeded'))/30
    prior=(at(duration-30,'client_window_succeeded')-at(duration-60,'client_window_succeeded'))/30
    # 调度滞后或积压增长属于不稳定证据，而不是新增的准入门槛。
    lag0=at(duration-30,'client_schedule_delay_max_us')
    lag1=at(duration,'client_schedule_delay_max_us')
    backlog0=at(duration-30,'client_in_flight')
    backlog1=at(duration,'client_in_flight')
    diverging=lag1-lag0>150000 or backlog1-backlog0>max(3,connections*.05)
    accounting=sent==completed+unresolved and len(latencies)==succeeded
    service_ok=all(v==0 for v in result.get('shutdown',{}).values()) and 'error' not in result
    bad_connections=sum(c['transport_failed']+c['logout_failed'] for c in clients)
    stable=(sent/(connections*20*duration)>=.99 and succeeded/max(sent,1)>=.999 and
            unresolved==0 and pct['p99'] is not None and pct['p99']<=200000 and
            last>=prior*.95 and not diverging and service_ok and not bad_connections and accounting)
    row=dict(connections=connections,duration=duration,valid=accounting,stable=stable,
             targetRps=connections*20,sent=sent,sendRps=sent/duration,
             windowSucceeded=success,successRps=success/duration,finalSucceeded=succeeded,
             attainment=sent/(connections*20*duration),successRate=succeeded/max(sent,1),
             errors=dict(errors),unresolved=unresolved,completed=completed,p95Us=p95,**{k+'Us':v for k,v in pct.items()},
             last30Rps=last,prior30Rps=prior,diverging=diverging,connectionFailures=bad_connections,
             transportFailures=sum(c['transport_failed'] for c in clients),
             logoutSucceeded=sum(c['logout_succeeded'] for c in clients),
             maxScheduleDelayUs=max(c['schedule_delay_max_us'] for c in clients),serviceOk=service_ok)
    resource_rows=[json.loads(l) for l in (work/'resources.jsonl').read_text().splitlines()]
    start=result['loadStartMonotonicMs']
    prev={};cpus=collections.defaultdict(list);rss=collections.defaultdict(int)
    for r in resource_rows:
        if not start<=r['monotonicMs']<=start+duration*1000:continue
        for p in r['processes']:
            role=p['role'];rss[role]=max(rss[role],p['rssBytes'])
            if role in prev:
                t,ticks=prev[role];dt=r['monotonicMs']-t
                if dt>0:cpus[role].append(100000*(p['cpuTicks']-ticks)/r['clockTicks']/dt)
            prev[role]=(r['monotonicMs'],p['cpuTicks'])
    row['cpuMeanPct']={k:sum(v)/len(v) for k,v in cpus.items() if v}
    row['rssPeakBytes']=dict(rss)
    matrix.write_json(work/'capacity-summary.json',row)
    for p in list(work.rglob('*.log'))+list(work.glob('*.latency-us.txt')):
        with p.open('rb') as src,gzip.open(str(p)+'.gz','wb') as dst:
            __import__('shutil').copyfileobj(src,dst)
        p.unlink()
    print('END',json.dumps(row),flush=True)
    return row


def report(batch,rows,reason):
    matrix.write_json(batch/'summary.json',dict(rows=rows,stopReason=reason))
    text=['# 3G2S capacity staircase','',
          'Release;20RPS/connection;15s initialization,120s preload timeout,5s idle,20s warmup,10s drain.',
          'Closed-loop single-in-flight walk; AOI+persistence,1s saves;4workers/Scene;all processes colocated.',
          'No admission/smoke/regression workload. Window success excludes warmup and drain.','',
          '|connections|seconds|target RPS|send RPS|success RPS|attainment|success rate|p99 ms|stable|',
          '|---:|---:|---:|---:|---:|---:|---:|---:|:---:|']
    for r in rows:
        if 'successRps' not in r:
            text.append(f'\nPreparation/instrumentation failure: {r}');continue
        text.append(f"|{r['connections']}|{r['duration']}|{r['targetRps']}|{r['sendRps']:.2f}|{r['successRps']:.2f}|{r['attainment']:.2%}|{r['successRate']:.3%}|{(r['p99Us'] or 0)/1000:.2f}|{r['stable']}|")
    eligible=[r for r in rows if r.get('valid') and 'successRps' in r]
    stable=[r for r in eligible if r['stable'] and r['duration']==120]
    text+=['',f'Stop: {reason}',
           'Peak observed: '+str(max(eligible,key=lambda r:r['successRps']) if eligible else None),
           '120s confirmed stable peak: '+str(max(stable,key=lambda r:r['successRps']) if stable else None),
           '', 'Underdelivery cannot establish open-loop server capacity. Busy/timeout count as failures. No automatic retries.']
    (batch/'report.md').write_text('\n'.join(text)+'\n')


def main():
    p=argparse.ArgumentParser();p.add_argument('--build',type=pathlib.Path,required=True);p.add_argument('--batch',type=pathlib.Path,required=True)
    a=p.parse_args();a.build=a.build.resolve();a.batch=a.batch.resolve();a.batch.mkdir(exist_ok=False,parents=True)
    assert 'CMAKE_BUILD_TYPE:STRING=Release' in (a.build/'CMakeCache.txt').read_text()
    for k in ('AOI_VERIFY_TOTAL','SCENE_QUEUE_DIAGNOSTICS'):os.environ.pop(k,None)
    signal.signal(signal.SIGTERM,matrix.interrupted);signal.signal(signal.SIGINT,matrix.interrupted)
    matrix.write_json(a.batch/'environment.json',dict(cpuCount=os.cpu_count(),
        binaries={n:hashlib.sha256((a.build/'bin'/n).read_bytes()).hexdigest() for n in ('sceneServer','gatewayServer','gateway_load_client')}))
    rows=[];reason='running';small=0
    try:
        schedule=[64,128,256,384,512,768,1024,1536,2048]
        while schedule:
            c=schedule.pop(0);r=measure(a.build,a.batch,c,60);rows.append(r);report(a.batch,rows,reason)
            if not r.get('valid') or not r.get('serviceOk') or r.get('transportFailures'):
                reason='preload/accounting/service/connection boundary';break
            if len(rows)>1:
                growth=r['successRps']/max(rows[-2]['successRps'],1)-1
                small=small+1 if growth<.05 and (r['diverging'] or r['attainment']<.99 or r['p99Us']>200000) else 0
                if small>=2:reason='two marginal throughput increments with degradation';break
            if int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))<1024*1024:
                reason='host available memory below1GiB';break
            if not schedule:
                schedule.append(int(c*1.5))
        valid=[r for r in rows if r.get('valid') and 'successRps' in r]
        if valid:
            peak=max(valid,key=lambda r:r['successRps']);stable=[r for r in valid if r['stable']]
            wanted={peak['connections']}
            if stable:wanted.add(stable[-1]['connections'])
            higher=[r['connections'] for r in valid if r['connections']>peak['connections']]
            if higher:wanted.add(min(higher))
            for c in sorted(wanted):
                rows.append(measure(a.build,a.batch,c,120));report(a.batch,rows,reason)
        matrix.write_json(a.batch/'completion.json',dict(cases=len(rows),stopReason=reason))
    finally:report(a.batch,rows,reason)


if __name__=='__main__':main()