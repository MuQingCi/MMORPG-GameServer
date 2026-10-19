#!/usr/bin/env python3
"""已批准的有界 256×20 复现批次，不自动升级容量。"""
import argparse
import gzip
import hashlib
import json
import os
import pathlib
import shutil
import signal
import time

import capacity_staircase as capacity
from busy_analysis import classify, stop_reason
from busy_resources import enrich

ROOT=pathlib.Path(__file__).resolve().parents[3]
matrix=capacity.matrix


def last_json(path):
    try:
        with path.open('rb') as stream:
            stream.seek(0,2)
            size=stream.tell()
            stream.seek(max(0,size-131072))
            lines=stream.read().splitlines()
        return json.loads(lines[-1]) if lines else None
    except (OSError,ValueError): return None


class Safety:
    def __init__(self):
        self.previous=None
        self.high_since=None
        self.last_success=None

    def check(self, rows, now):
        if len(rows)!=3 or any(r is None for r in rows): return None
        if min(r['elapsedMs'] for r in rows)<1000: return None
        sent=sum(r['client_sent'] for r in rows)
        errors=sum(r['client_errors'] for r in rows)
        success=sum(r['client_business_succeeded'] for r in rows)
        if self.previous is None:
            self.previous=(sent,errors,success)
            self.last_success=now
            return None
        a,b,c=self.previous
        ds,de=sent-a,errors-b
        if success>c: self.last_success=now
        if ds>0 and de/ds>.1:
            if self.high_since is None: self.high_since=now
        else: self.high_since=None
        self.previous=(sent,errors,success)
        return stop_reason(ds,de,now-self.high_since if self.high_since is not None else 0,now-self.last_success)


def verify(work, row):
    expected={}
    for path in work.glob('load-gw*.json.players.jsonl'):
        for line in path.read_text().splitlines():
            p=json.loads(line); expected[p['playerId']]=p
    def table(name):
        return {int(v[0]):list(map(int,v[1:])) for v in
                (line.split() for line in (work/name).read_text().splitlines() if line.strip())}
    db=table('final-db.tsv')
    bags=table('final-bags.tsv')
    manifest=json.loads((work/'players.json').read_text())
    snapshots=all(p['playerId'] in expected and db.get(p['playerId'],[])[:2]==
                  [expected[p['playerId']]['x'],expected[p['playerId']]['y']] and
                  len(db[p['playerId']])==4 and db[p['playerId']][2]>7 and db[p['playerId']][3]==p['sceneId'] for p in manifest)
    bag_ok=all(bags.get(p['playerId'])==[101,0,9] for p in manifest)
    result=json.loads((work/'result.json').read_text())
    clients=[json.loads(p.read_text()) for p in work.glob('load-gw*.json')]
    accounting=all(c['sent']==c['completed']+c['unresolved'] for c in clients)
    value=dict(accounting=accounting,latencySamplesMatched=row['valid'],logout=row['logoutSucceeded'],
               acknowledgedPlayers=len(expected),finalSnapshotsMatched=snapshots,bagsMatched=bag_ok,
               onlineSaveEvidence=result.get('onlineSaveEvidence'),shutdown=result.get('shutdown'),
               cleanup=result.get('cleanupExitCodes'),unresolved=row['unresolved'])
    matrix.write_json(work/'verification.json',value)
    return accounting and row['valid'] and len(manifest)==256 and len(expected)==256 and row['logoutSucceeded']==256 and snapshots and bag_ok and row['unresolved']==0 and row['serviceOk'] and (result.get('onlineSaveEvidence') or {}).get('advancedPlayers')==256


def run_one(build,batch,label,diagnostic,duration):
    target=batch/label
    target.mkdir(exist_ok=False)
    os.environ['SCENE_BUSY_DIAGNOSTICS']='1'
    os.environ.pop('SCENE_QUEUE_DIAGNOSTICS',None)
    os.environ.pop('AOI_VERIFY_TOTAL',None)
    if diagnostic: os.environ['SCENE_QUEUE_DIAGNOSTICS']='1'
    matrix.write_json(target/'environment.json',dict(diagnostic=diagnostic,busyDiagnostic=True,
        experimentalActiveIndex=os.environ.get('SCENE_EXPERIMENT_ACTIVE_DB_INDEX')=='1',
        connections=256,rps=20,duration=duration,gateways=3,scenes=2,workerThreads=4,flushIntervalMs=1000,
        cpuCount=os.cpu_count(),binaries={n:hashlib.sha256((build/'bin'/n).read_bytes()).hexdigest()
                                       for n in ('sceneServer','gatewayServer','gateway_load_client')}))
    original_sql,original_stop=matrix.sql,matrix.Runner.stop
    original_sample=matrix.Runner.sample
    original_processes=matrix.sample_processes
    context={}
    captured=False
    safety=Safety()

    def sql(ctx,statement):
        context.update(ctx)
        return original_sql(ctx,statement)

    def stop(runner,selected=None):
        nonlocal captured
        if not captured and any(n.startswith('scene') for n,p in (runner.processes if selected is None else selected)):
            captured=True
            try:
                for name,statement in (
                    ('final-db.tsv','SELECT id,pos_x,pos_y,save_version,scene_id FROM matrix_test.player ORDER BY id;'),
                    ('final-bags.tsv','SELECT player_id,item_id,slot,quantity FROM matrix_test.player_item_stack ORDER BY player_id;')):
                    (runner.work/name).write_text(original_sql(context,statement)+'\n')
            except Exception as error:
                matrix.write_json(runner.work/'capture-error.json',dict(error=type(error).__name__))
        return original_stop(runner,selected)

    def sample(runner,output):
        original_sample(runner,output)
        records=[last_json(runner.work/f'metrics-gw{i}.jsonl') for i in range(1,4)]
        reason=safety.check(records,time.monotonic())
        if shutil.disk_usage(runner.work).free<1024**3: reason='disk-below-1GiB'
        if reason:
            matrix.write_json(runner.work/'safety-stop.json',dict(reason=reason,monotonicUs=int(time.monotonic()*1e6)))
            for name,process in runner.processes:
                if name.startswith('client') and process.poll() is None: process.send_signal(signal.SIGTERM)
            raise RuntimeError('safety-stop: '+reason)

    matrix.sql, matrix.Runner.stop, matrix.Runner.sample=sql,stop,sample
    matrix.sample_processes=lambda roots: enrich(original_processes(roots))
    try:
        row=capacity.measure(build,target,256,duration)
    finally:
        matrix.sql,matrix.Runner.stop,matrix.Runner.sample=original_sql,original_stop,original_sample
        matrix.sample_processes=original_processes
    matrix.write_json(target/'summary.json',row)
    work=target/f'c256-d{duration}'
    if 'successRps' not in row or (work/'safety-stop.json').exists():
        return dict(label=label,row=row,verified=False,busy=None,stop=True)
    ok=verify(work,row)
    start=json.loads((work/'result.json').read_text())['loadStartMonotonicMs']*1000
    traces=[]
    for path in sorted(work.glob('*.busy.jsonl')):
        records=[json.loads(line) for line in path.read_text().splitlines()]
        trace=classify(records,start,start+duration*1000000)
        trace['source']=path.name
        traces.append(trace)
    client_busy=sum(c.get('business_errors',{}).get('1011:server busy',0) for c in
                    (json.loads(p.read_text()) for p in work.glob('load-gw*.json')))
    complete=len(traces)==2 and all(t['complete'] for t in traces)
    server_busy=sum(sum(t['windowCounts']) for t in traces)
    matrix.write_json(work/'busy-analysis.json',dict(scenes=traces,clientBusy=client_busy,
        serverWindowBusy=server_busy,complete=complete,
        caveat='Server event window differs from client request-send window; correlate individual events before claiming equality.'))
    # 预热或排空期间的拒绝也应触发检查，而非直接启动长时用例。
    all_busy=sum(sum(t['finalCounts']) for t in traces)
    return dict(label=label,row=row,verified=ok,busy=client_busy+all_busy,stop=not ok or not complete)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=pathlib.Path,required=True)
    parser.add_argument('--batch',type=pathlib.Path,required=True)
    args=parser.parse_args()
    build,batch=args.build.resolve(),args.batch.resolve()
    batch.mkdir(exist_ok=True,parents=True)
    assert not (batch/'source').exists(),'refusing repeated batch'
    assert 'CMAKE_BUILD_TYPE:STRING=Release' in (build/'CMakeCache.txt').read_text()
    for folder in ('Zone/sceneServer/src-cpp','Zone/sceneServer/config','Zone/sceneServer/lua_script',
                   'Zone/sceneServer/tests','Zone/gatewayServer/tests','Zone/config','Zone/proto'):
        shutil.copytree(ROOT/folder,batch/'source'/folder,ignore=shutil.ignore_patterns('__pycache__'))
    for name in ('sceneServer','gatewayServer','gateway_load_client'):
        (batch/'binary').mkdir(exist_ok=True)
        shutil.copy2(build/'bin'/name,batch/'binary'/name)
    signal.signal(signal.SIGTERM,matrix.interrupted)
    signal.signal(signal.SIGINT,matrix.interrupted)
    rows=[]
    schedule=[('minimal-1',False,120),('full-1',True,120),('full-2',True,120),('minimal-2',False,120)]
    reason='running'
    for label,diagnostic,duration in schedule:
        value=run_one(build,batch,label,diagnostic,duration)
        rows.append(value)
        matrix.write_json(batch/'progress.json',dict(rows=rows,reason=reason))
        if value['stop'] or value['busy']:
            reason='evidence-or-correctness-gate';break
    else:
        for i in (1,2):
            value=run_one(build,batch,f'long-{i}',False,600)
            rows.append(value)
            matrix.write_json(batch/'progress.json',dict(rows=rows,reason=reason))
            if value['stop'] or value['busy']:
                reason='evidence-or-correctness-gate';break
        else: reason='approved-budget-exhausted-no-busy'
    matrix.write_json(batch/'completion.json',dict(rows=rows,reason=reason))


if __name__=='__main__': main()