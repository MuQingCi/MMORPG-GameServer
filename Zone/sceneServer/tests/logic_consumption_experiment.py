#!/usr/bin/env python3
"""显式执行已批准的诊断/A-B 实验，绝不重试或覆盖用例。"""
import argparse
import gzip
import hashlib
import json
import os
import pathlib
import shutil
import signal
import subprocess
import time

import benchmark_matrix as matrix


def formal_schedule():
    return [(variant,c,d,rep) for c,d in ((256,30),(512,30),(512,60))
            for rep in range(1,4) for variant in (('A','B') if rep%2 else ('B','A'))]


def run(build, batch, connections, duration, diagnostic=False):
    assert not batch.exists(), f'refusing overwrite: {batch}'
    batch.mkdir(parents=True)
    case=dict(gateways=3, scenes=2, connections=connections, rps=20, persistence=True, duration=duration)
    matrix.write_json(batch/'matrix.json',[case])
    matrix.write_json(batch/'environment.json',dict(build=str(build),diagnostic=diagnostic,
        binaries={n:hashlib.sha256((build/'bin'/n).read_bytes()).hexdigest()
                  for n in ('sceneServer','gatewayServer','gateway_load_client')}))
    original=matrix.Runner.start
    def start(runner,name,args):
        handle=(runner.work/(name+'.stdout.log')).open('w');runner.handles.append(handle)
        env=os.environ.copy();env.pop('SCENE_QUEUE_DIAGNOSTICS',None)
        if diagnostic and name.startswith('scene'):env['SCENE_QUEUE_DIAGNOSTICS']='1'
        process=subprocess.Popen(args,cwd=runner.work,stdout=handle,stderr=subprocess.STDOUT,env=env)
        runner.processes.append((name,process));return process
    matrix.Runner.start=start
    sample=matrix.Runner.sample
    profiled=False
    profiles=[]
    def prof_sample(runner,output):
        nonlocal profiled
        sample(runner,output)
        barrier=runner.work/'start-barrier/START'
        if diagnostic and not profiled and barrier.exists() and time.monotonic()*1000>int(barrier.read_text())+3000:
            profiled=True
            pids=','.join(str(p.pid) for n,p in runner.processes if n.startswith('scene'))
            log=(runner.work/'perf.log').open('w');runner.handles.append(log)
            profiles.append(subprocess.Popen(['perf','record','-F','49','-g','-e','cpu-clock','-p',pids,
                '-o',str(runner.work/'perf.data'),'--','sleep','8'],stdout=log,stderr=log))
    matrix.Runner.sample=prof_sample
    print('START',batch.name,case,flush=True)
    try:
        result=matrix.run_case(build,batch/matrix.identity(case),case,0)
    finally:
        matrix.Runner.start=original;matrix.Runner.sample=sample
        for p in profiles:
            if p.poll() is None:p.terminate()
            p.wait(timeout=15)
    work=batch/matrix.identity(case)
    if (work/'perf.data').exists():
        report=subprocess.run(['perf','report','--stdio','--no-children','-i',str(work/'perf.data'),
                               '--percent-limit','1'],capture_output=True,text=True)
        (work/'perf-report.txt').write_text(report.stdout+report.stderr)
    for path in list(work.rglob('*.log'))+list(work.glob('*.latency-us.txt')):
        with path.open('rb') as src,gzip.open(str(path)+'.gz','wb') as dst:shutil.copyfileobj(src,dst)
        path.unlink()
    matrix.aggregate(batch)
    matrix.write_json(batch/'completion.json',dict(completed=1))
    print('END',batch.name,'passed=',result['passed'],result.get('error',''),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=pathlib.Path,required=True)
    parser.add_argument('--candidate',type=pathlib.Path)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    parser.add_argument('--diagnostic',action='store_true')
    args=parser.parse_args()
    signal.signal(signal.SIGTERM,matrix.interrupted);signal.signal(signal.SIGINT,matrix.interrupted)
    if args.diagnostic:
        for c in (256,512):run(args.baseline,args.output/f'diagnostic-c{c}',c,30,True)
    else:
        assert args.candidate
        for variant,c,d,rep in formal_schedule():
            build=args.baseline if variant=='A' else args.candidate
            run(build,args.output/f'{variant}-c{c}-d{d}-rep{rep}',c,d)


if __name__=='__main__':main()