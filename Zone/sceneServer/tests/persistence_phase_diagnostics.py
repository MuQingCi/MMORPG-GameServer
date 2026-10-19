#!/usr/bin/env python3
"""显式、有界的原负载诊断，不干预阶段，不自动重试。"""
import argparse
import hashlib
import json
import pathlib
import time

from analyze_queue_diagnostics import summarize, phase_samples
from persistence_load_e2e import main as persistence_run
from persistence_staircase import client_passed


def run_batch(repeats, run_one):
    if not 1<=repeats<=3:
        raise ValueError('repeats must be in 1..3')
    report=dict(passed=True,runs=[])
    for index in range(1,repeats+1):
        try:
            result=run_one(index)
        except Exception as error:
            result=dict(passed=False,failure=str(error))
        report['runs'].append(result)
        if not result['passed']:
            report.update(passed=False,stoppedAt=index)
            break
    return report


def analyze_run(path, require_complete=True):
    work=path/'run'
    metrics=[json.loads(line) for line in (work/'metrics-gw1.jsonl').read_text().splitlines()]
    start=metrics[0]['readMonotonicUs']
    reports={}
    for scene in (10,20):
        file=work/f'scene{scene}.metrics.json.queues.jsonl'
        rows=[json.loads(line) for line in file.read_text().splitlines()]
        report=summarize(rows)
        issues=[]
        def check(condition, message):
            if not condition:
                issues.append(message)
        check(all(a['monotonicUs']<=b['monotonicUs'] for a,b in zip(rows,rows[1:])), 'nonmonotonic diagnostic timestamps')
        check(rows[-1].get('fullSnapshot'), 'missing final diagnostic snapshot')
        check(not report['writeFailed'] and report['droppedEvents']==0, 'invalid diagnostic output')
        check(bool(report['waits']) and not any(w['missing'] for w in report['waits']), 'missing queue timestamps or histograms')
        check(all(w['valid'] for w in report['waitWindows']), 'reset or invalid histogram')
        check(bool(report['tasks']) and all(not t['missing'] and not t['duplicateStages'] for t in report['tasks']), 'incomplete save correlation')
        check(all(t.get('dbQueueUs',-1)>=0 and t.get('executeUs',-1)>=0 and t.get('returnAndApplyUs',-1)>=0 for t in report['tasks']), 'invalid save stage ordering')
        load=[r for r in rows if start<=r['monotonicUs']<start+30000000]
        check(bool(load) and load[0]['monotonicUs']-start<=30000 and start+30000000-load[-1]['monotonicUs']<=30000, 'load observation coverage incomplete')
        report['validationIssues']=issues
        report['load']=summarize(load) if load else dict(samples=0,emptySamples=0,maxDepth=0,samplingGaps=0)
        report['offlinePhases']=[phase_samples(rows,start,30,offset) for offset in (0,17000,37000)]
        file.with_suffix('.summary.json').write_text(json.dumps(report,indent=2)+'\n')
        reports[str(scene)]=dict(valid=not issues,validationIssues=issues,samples=report['load']['samples'],emptySamples=report['load']['emptySamples'],
            maxDepth=report['load']['maxDepth'],samplingGaps=report['load']['samplingGaps'],
            saveTasks=len(report['tasks']),offlinePhases=report['offlinePhases'])
    if require_complete and any(not r['valid'] for r in reports.values()):
        raise ValueError('diagnostic validation failed: '+json.dumps(reports))
    return reports


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build',type=pathlib.Path)
    parser.add_argument('--repeats',type=int,choices=(1,2,3),default=3)
    args=parser.parse_args(argv)
    build=args.build.resolve()
    evidence=build/('persistence-phase-'+str(time.time_ns()))
    evidence.mkdir(mode=0o700)
    config=dict(connectionsPerGateway=64,totalConnections=128,rps=10,duration=30,drain=10,
                queueSeconds=3,repeats=args.repeats,diagnostics=True)
    config['binarySha256']={name:hashlib.sha256((build/'bin'/name).read_bytes()).hexdigest()
                            for name in ('sceneServer','gatewayServer','gateway_load_client')}
    (evidence/'configuration.json').write_text(json.dumps(config,indent=2)+'\n')
    print('Batch evidence:',evidence,flush=True)
    def run(index):
        path=evidence/f'run-{index}'
        result=dict(passed=False,index=index,evidence=str(path))
        try:
            persistence_run(str(build),connections=64,rps=10,duration=30,drain=10,
                            queue_seconds=3,evidence_path=path,queue_diagnostics=True)
            clients=[json.loads((path/f'run/load-gw{g}.json').read_text()) for g in (1,2)]
            assert all(client_passed(c,10,30) for c in clients), 'client admission gate'
            result['scenes']=analyze_run(path)
            result['passed']=True
        except Exception as error:
            result['failure']=str(error)
            # 尽可能保留部分汇总，但不得掩盖原始失败。
            try:
                result['scenes']=analyze_run(path,require_complete=False)
            except Exception as analysis_error:
                result['analysisFailure']=str(analysis_error)
        (evidence/f'result-{index}.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result),flush=True)
        return result
    report=run_batch(args.repeats,run)
    report['configuration']=config
    (evidence/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())