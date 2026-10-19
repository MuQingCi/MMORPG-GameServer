#!/usr/bin/env python3
"""独立审计原始汇总，生成固定 18 用例 A/B 报告（仅使用标准库）。"""
import argparse
import collections
import gzip
import hashlib
import json
import math
import pathlib
import statistics

import benchmark_audit


def expected_names():
    return {f'{v}-c{c}-d{d}-rep{r}' for c,d in ((256,30),(512,30),(512,60))
            for v in ('A','B') for r in (1,2,3)}


def verify(work):
    audit=benchmark_audit.audit_case(work)
    result=json.loads((work/'result.json').read_text())
    cached=json.loads((work/'case-summary.json').read_text())
    clients=[json.loads(p.read_text()) for p in work.glob('load-gw*.json')]
    for key in ('load_attempted','sent','completed','business_succeeded','errors','unresolved','logout_succeeded'):
        if cached[key]!=sum(c[key] for c in clients):audit['issues'].append('cached total mismatch: '+key)
    duration=result['case']['duration']
    for key,total in (('actualSendRps','sent'),('successfulRps','business_succeeded')):
        if not math.isclose(cached[key],sum(c[total] for c in clients)/duration):
            audit['issues'].append('cached RPS mismatch: '+key)
    counts=collections.Counter()
    for log in work.rglob('*.log.gz'):
        with gzip.open(log,'rt',errors='replace') as f:
            for line in f:
                for marker in ('sendToWorker: queue full','drop player request: worker queue full','shutdown persistence drain incomplete'):
                    if marker in line:counts[marker]+=1
    return cached,dict(**audit,logCounts=dict(counts))


def report(batch,require_complete=False):
    cases={}
    audits=[]
    observed=set()
    for directory in sorted(batch.iterdir()):
        if not directory.is_dir() or not directory.name.startswith(('A-','B-')):continue
        observed.add(directory.name)
        files=list(directory.glob('*/result.json'))
        if not files:continue
        if len(files)!=1:raise ValueError('unexpected result count: '+directory.name)
        row,audit=verify(files[0].parent);cases[directory.name]=row;audits.append(dict(batch=directory.name,**audit))
    if require_complete and (set(cases)!=expected_names() or observed!=expected_names()):
        raise ValueError('A/B exact case set incomplete or extra batch present')
    valid=not any(a['issues'] for a in audits)
    summary=dict(completed=len(cases),expected=18,valid=valid,cases=audits)
    (batch/'ab-independent-audit.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
    text='''# 逻辑线程消费能力诊断与单因素 A/B 测试

## 范围与结论边界

3 Gateway / 2 Scene；每Scene4逻辑线程；每GW2IO线程；每(Scene,GW)2条后端通道。
20RPS/连接，持久化开启、1000ms flush；Release -O3 -DNDEBUG -g1、info日志；隔离MariaDB/Redis。
队列初始容量保持1024，未增加线程、扩大队列或更改路由。客户端闭环单在途，START后约250ms首发，10秒drain。
详细诊断2组（256/512×30s，各1次），含49Hz、8秒Scene CPU profiling；不用于正式性能比较。
正式A/B：256×30s、512×30s、512×60s，每版本各3次，共18组。
A为改动前冻结基线；B仅业务改动为借用只读AOI已提交基线，避免每观察者每tick复制整张unordered_map。
B保留可选诊断实现，但正式A/B都关闭详细诊断与profiling；因此非零的诊断关闭路径开销也是对比限制。
按每档rep1 A/B、rep2 B/A、rep3 A/B串行运行；无自动重试/覆盖/调参。

## 诊断证据

256档两个Scene的AOI/恢复阶段累计墙钟时间约11.57/11.69秒；512档约67.58/64.57秒。
每Scene合计4worker，不是单核CPU时间，不将墙钟时间占比当CPU占比。
512档handler合计约10.25/11.21秒，分类入队拒绝334/3947次，10ms采样worker队列峰值901/937。
固定类别计数区分业务、DB结果、内部Service、定时器、生命周期和其他；范围边界偏移见work-diagnostic-summary.json。
perf短窗显示AOI视野查询、客户端视野组装和哈希表复制等热点；支持选择减少AOI基线复制，不证明它是唯一根因。
维护阶段还含AOI推送与恢复；生产者分块、同机日志/网络和调度竞争仍可能影响结果。

## 正式逐次结果

|版本/案例|通过|发送/目标 RPS|成功摊销 RPS|p50/p90/p99 ms|错误/未解决|队列满日志行|
|---|---|---|---|---|---|---|
'''
    byname={a['batch']:a for a in audits}
    for name,row in sorted(cases.items()):
        latency='/'.join(f'{row[k]/1000:.3f}' if row[k] is not None else 'NA' for k in ('p50Us','p90Us','p99Us'))
        count=byname[name]['logCounts'].get('sendToWorker: queue full',0)
        text+=f"|{name}|{row['passed']}|{row['actualSendRps']:.2f}/{row['targetRps']}|{row['successfulRps']:.2f}|{latency}|{row['errors']}/{row['unresolved']}|{count}|\n"
    text+='\n## 逐次资源与补充指标\n\n|案例|时窗成功RPS|调度延误均值/最大 ms|服务CPU均值和 %|服务RSS峰值之和 MiB|\n|---|---:|---|---:|---:|\n'
    for name,row in sorted(cases.items()):
        text+=f"|{name}|{row['loadWindowSuccessRps']:.2f}|{row['meanScheduleDelayUs']/1000:.3f}/{row['maxScheduleDelayUs']/1000:.3f}|{row['serviceCpuMeanPct']:.2f}|{row['serviceRssPeakSumBytes']/1048576:.2f}|\n"
    text+='\n## 中位数对比与门槛\n\n|档位|A/B通过数|发送RPS中位数 A→B|成功RPS中位数 A→B|p99中位数ms A→B|B相对A发送变化|\n|---|---|---|---|---|---|\n'
    medians=[]
    for c,d in ((256,30),(512,30),(512,60)):
        groups={v:[cases[f'{v}-c{c}-d{d}-rep{rep}'] for rep in (1,2,3)
                   if f'{v}-c{c}-d{d}-rep{rep}' in cases] for v in ('A','B')}
        if any(len(g)!=3 for g in groups.values()):continue
        values={v:{k:statistics.median(r[k] for r in g) for k in ('actualSendRps','successfulRps','p99Us')} for v,g in groups.items()}
        a,b=values['A'],values['B'];change=(b['actualSendRps']/a['actualSendRps']-1)*100
        text+=f"|{c}×{d}s|{sum(r['passed'] for r in groups['A'])}/3 → {sum(r['passed'] for r in groups['B'])}/3|{a['actualSendRps']:.2f} → {b['actualSendRps']:.2f}|{a['successfulRps']:.2f} → {b['successfulRps']:.2f}|{a['p99Us']/1000:.3f} → {b['p99Us']/1000:.3f}|{change:+.2f}%|\n"
        medians.append(dict(connections=c,duration=d,values=values,sendChangePct=change,
                            regressionGate=(b['actualSendRps']>=a['actualSendRps']*.95 and b['p99Us']<=a['p99Us']*1.1) if c==256 else None))
    text+='\n## 单次范围（不隐藏离散程度）\n\n|档位/版本|发送RPS最小~最大|p99 ms最小~最大|\n|---|---|---|\n'
    for c,d in ((256,30),(512,30),(512,60)):
        for v in ('A','B'):
            rows=[row for name,row in cases.items() if name.startswith(f'{v}-c{c}-d{d}-')]
            if rows:
                text+=f"|{c}×{d}s/{v}|{min(r['actualSendRps'] for r in rows):.2f}~{max(r['actualSendRps'] for r in rows):.2f}|{min(r['p99Us'] for r in rows)/1000:.3f}~{max(r['p99Us'] for r in rows)/1000:.3f}|\n"
    text+='''
## 口径、正确性与限制

- 发送RPS=业务sent/发压秒数；成功摊销RPS=business_succeeded/发压秒数，含drain回包，不冒充时窗成功RPS。
- p50/p90/p99由全部网关成功原始延迟合并最近秩法重算；失败和未完成不在延迟样本中。
- attempted=completed+unresolved；错误与未解决分列，队列满日志含内部投递与重试，不等于失败客户端请求数。
- 每案例case-summary.json含时窗成功RPS、调度延误；resource-summary.json含进程CPU/RSS/PSS/IO，resources.jsonl保留线程累计CPU ticks。CPU单核=100%；RSS峰值之和非同时峰值。
- 网络lo为命名空间统计，不能归因单进程；本环境lo未变化，不能据零值推断无流量。
- passed须客户端/Scene退出正常、全玩家在线版本提升、最终位置/scene/version/道具一致、每占用(GW,Scene)重登验证。失败缺失证明不能视为通过。
- 256门槛：B发送RPS中位数>=A的95%、p99中位数<=A的110%；512完整支持须全部正确性证明通过，20RPS/连接还须发送>=目标98%。
- 三次重复报告中位数但不是统计显著性证明；未绑核、同机DB/Redis/日志/客户端，不能推导生产容量。
- 固定直方图仅提供桶边界；诊断快照非全局原子，等待统计不包含未出队消息；监测可能漏掉瞬时峰值。
- 不存在外部独立代理评审；本轮采用本地代码审查和独立原始数据复算。

## 背压路径审计

- 玩家普通请求：worker入队失败告警并丢弃，可能无业务回复；生命周期通知积压非空时也会阻止普通请求投递，影响范围需后续专项修复。
- 生命周期ONLINE/OFFLINE：进入pendingClientNotifications_重试；本次不改策略，仍需后续审查积压上界与跨玩家影响。
- DB结果：DbResultLedger保留结果并重投；事务完成不等同于结果已应用，保留epoch/生命周期校验。
- 定时器：投递失败记录drop，不阻塞定时器线程，不保证此次回调送达。
- AOI跨worker发布：按目标记录失败，Service维护重投/恢复；客户端AOI推送仅成功入网络队列后提交基线。
- 本轮未改背压语义：减少AOI只读副本不能解决所有静默丢弃、拒绝回复或可靠投递问题。

## 复现与归档

复现入口：/home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/logic_consumption_experiment.py。
审计入口：/home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/report_logic_consumption.py --batch <本批绝对路径> --require-complete。
各子批次environment.json保存冻结二进制SHA256；ledger.json保存TDD、选择依据、偏离裁决。
归档包含源码快照、补丁、CMakeCache、环境、两组诊断、18组原始结果、压缩日志及清理清单。
历史批次不覆盖；现有Git忽略规则不修改；不自动提交。
'''
    (batch/'ab-medians.json').write_text(json.dumps(medians,indent=2)+'\n')
    (batch/'report.md').write_text(text)
    return summary


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--batch',type=pathlib.Path,required=True)
    parser.add_argument('--require-complete',action='store_true')
    args=parser.parse_args();summary=report(args.batch,args.require_complete)
    print(json.dumps(dict(completed=summary['completed'],valid=summary['valid'])))
    raise SystemExit(0 if summary['valid'] else 1)