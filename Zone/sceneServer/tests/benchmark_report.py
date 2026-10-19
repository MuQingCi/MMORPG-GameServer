#!/usr/bin/env python3
"""仅渲染实际观测的矩阵数据；归档并校验证据，绝不删除旧运行。"""
import argparse
import collections
import gzip
import hashlib
import json
import pathlib
import shutil
import time

from benchmark_matrix import ROOT, ZONE, aggregate, exact_case_set


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--batch', required=True, type=pathlib.Path)
    parser.add_argument('--archive', action='store_true')
    args = parser.parse_args()
    batch = args.batch.resolve()
    rows = aggregate(batch)
    selected = json.loads((batch / 'matrix.json').read_text())
    complete = (batch / 'completion.json').exists() and exact_case_set(selected, [r['id'] for r in rows])
    expected_count = len(selected)
    archive = ROOT / 'docs/benchmark-results/阶梯测试数据V1' / batch.name
    evidence = batch
    if args.archive:
        assert complete, 'refuse final archive of incomplete matrix'
        assert not archive.exists(), 'archive already exists; verify instead of overwriting'
        assert not any(p.is_symlink() for p in batch.rglob('*')), 'symlink in evidence'
        shutil.copytree(batch, archive)
        checksums = []
        for source in sorted(batch.rglob('*')):
            if source.is_file():
                relative = source.relative_to(batch)
                value = digest(source)
                assert digest(archive / relative) == value, str(relative)
                checksums.append(f'{value}  {relative.as_posix()}')
        (archive / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')
        evidence = archive
    passed = sum(r['passed'] for r in rows)
    accounting = sum(r['accountingValid'] and r['latencyCountValid'] for r in rows)
    root, zone = str(ROOT), str(ZONE)
    text = f'''# 阶梯测试数据 V1

更新时间：{time.strftime('%Y-%m-%d %H:%M:%S %z')}。

## 状态与结论边界

**{'正式矩阵已完成' if complete else '正式矩阵进行中，以下仅为已完成案例，不能据此宣称全部完成'}**。
已完成 **{len(rows)}/{expected_count}**，请求/生命周期/持久化验收通过 **{passed}**，
请求守恒且延迟样本数一致 **{accounting}/{len(rows)}**。
失败案例保留，不以重跑覆盖。验收通过不等于达到目标吞吐或满足延迟 SLO。
每组执行一次，不计算置信区间，不作统计显著性声明。

证据目录：`{evidence}`。
{'归档副本已逐文件 SHA-256 比对，清单为 SHA256SUMS。' if args.archive else '尚未执行最终归档；运行中的文件不得清理。'}
报告和证据受到现有 `docs/*` 忽略规则影响，未修改规则、未强制加入 Git 索引。

## 矩阵与固定条件

- Gateway/Scene：固定 3/2；总连接 128/256/512；网关分配分别为 43/43/42、86/85/85、171/171/170。
- 每连接目标 20 RPS；持久化开启；发压 30/60 秒；正式测试仅 6 组，各1次。
- 正式发压累计 270 秒，另有连接准备、数据库初始化、10 秒 drain、校验和停止时间。
- Release 独立构建：`{zone}/build-benchmark-3g2s`；每 Gateway 2 IO 线程，每 Scene 4 logic 线程；info 日志。
- 每个 (Scene, Gateway) 2 个 channel；仅生成配置设置帧限：C×R>10000 时 100000，否则 10000。
  原始配置不修改；scalar 替换要求只出现一次，不追加重复 YAML net/maxFramesPerSec。
- 持久化开启，1000ms flush 配置；每案例创建全新本机临时 MariaDB/Redis，不连接业务库。
- 每案例独立端口、目录和玩家 ID；数据库密码仅临时生成，归档配置已脱敏。
- 在正式时窗前完成 Auth/Enter 和全局 READY；START 使用 CLOCK_MONOTONIC；客户端首发在 START+250ms，
  因而目标时窗包含固定 250ms 起步间隔，不隐去这一吞吐偏差。没有额外稳态预热。
- 发压器是闭环、每连接最多一个在途移动；CPU/日志/数据库与发压器同机竞争资源。
- 旧 Debug 结果与已作废480组批次均不混入本次结果。

## 指标口径、来源和局限

|指标|原始来源与计算|范围/时间窗/单位/局限|
|---|---|---|
|目标 RPS|case.json，C×R|总连接，不是每网关连接；单位 req/s|
|实际发送 RPS|load-gw*.json sent 求和 / duration|业务请求成功写入 socket；不含 Auth/Enter/Logout；不等于服务端收到|
|成功摊销 RPS|business_succeeded 求和 / duration|含 drain 回包，按发压时长摊销；不冒充仅时窗内完成率|
|时窗成功 RPS|metrics-gw*.jsonl 在 elapsedMs=duration×1000 的 client_business_succeeded 总和 / duration|1 秒采样，实际 readMonotonicUs 有调度偏移|
|成功率/达成率|成功数/attempted；sent/(C×R×duration)|分别衡量正确性和目标吞吐，不能相互替代|
|请求终态|load_attempted = completed + unresolved|completed 含成功/业务错误/传输错误/超时/解码错误；准备或 logout 错误可能使原工具计数不守恒，显式标记 accountingValid=false，不能隐藏|
|p50/p90/p99|load-gw*.json.latency-us.txt.gz 全局合并排序，rank=ceil(p×N)-1|微秒，成功业务回复的 socket send→匹配回复；含 drain，不含失败/未完成；非所有请求的无偏延迟；不平均客户端分位数|
|调度延误|schedule_delay_us/sent，schedule_delay_max_us 最大值|微秒，实际 send 后时间减计划 nextSend；闭环延误不是 RTT|
|CPU|resources.jsonl，/proc/PID/stat utime+stime 差 / CLK_TCK / Δt ×100|进程含线程；单核=100%，可超过100%；仅 START..duration 内约1秒采样，角色平均/采样区间峰值；非瞬时峰值|
|线程 CPU|resources.jsonl processes[].threads 中 /proc/PID/task/TID/stat|原始累计 ticks；线程 ID 生命周期不可混合，不能再加到进程 CPU|
|RSS/PSS|/proc/PID/stat rss×页大小；smaps_rollup Pss×1024|bytes；每角色平均/峰值；RSS 求和重复计算共享页，PSS 权限不足为 null；各进程峰值之和并非同时峰值|
|磁盘 IO|/proc/PID/io read_bytes/write_bytes 差 / Δt|bytes/s，采样区间平均；缓存命中和延后写回不等于应用读写；DB 角色单列|
|网络|/proc/net/dev lo RX/TX 累计差 / Δt|同网络命名空间，不是每进程；包含并发其他流量、客户端和DB；不能 RX+TX 当唯一业务流量|
|业务字节|load summary bytes_sent/bytes_received|原客户端统计口径：发送含帧，接收成功 body；不能对称相加推导网卡吞吐|
|队列/丢弃|metrics-gw*.jsonl 的 gateway_*/scene*，metrics-summary.json first/last/peak|1 秒快照；counter 看末减初，gauge 看峰值；短暂峰值可能漏采，多个客户端重复采 Scene 不可求和|
|持久化|final-db.tsv、final-bags.tsv、persistence-verification.json、result.json|每玩家最终位置对齐已确认移动，version>7、scene匹配、道具保持；每 (GW,Scene) 选1人重新登录，另检在线 version 提升和 Scene graceful exit；不是故障恢复/断电耐久测试|

资源汇总逐案例见 resource-summary.json；没有可读样本时为 null，不能解释成零。
日志压缩保留；资源采样和 info 日志开销属于本基线的一部分。未绑核、未控制宿主机其他负载，
单次结果用于该机器下定位瓶颈，不推断生产容量或横向扩展线性度。

## 已验证的复现入口

以下命令在项目目录执行，所有路径为绝对路径；源码脚本不依赖构建目录中的临时脚本。

```bash
cd {root}
command -v mariadbd mariadb-install-db mariadb redis-server pidstat perf iostat sar vmstat
df -h {root} /tmp
free -m
nproc
cmake -S {zone} -B {zone}/build-benchmark-3g2s -DCMAKE_BUILD_TYPE=Release
cmake --build {zone}/build-benchmark-3g2s -j6
ctest --test-dir {zone}/build-benchmark-3g2s --output-on-failure
python3 -B {zone}/sceneServer/tests/test_benchmark_matrix.py
python3 -B {zone}/sceneServer/tests/benchmark_matrix.py --build {zone}/build-benchmark-3g2s --batch {zone}/build-acceptance/benchmark-3g2s-smoke --smoke --resume
python3 -B {zone}/sceneServer/tests/benchmark_matrix.py --build {zone}/build-benchmark-3g2s --batch {batch} --approved-3g2s --resume
python3 -B {zone}/sceneServer/tests/benchmark_matrix.py --build {zone}/build-benchmark-3g2s --batch {batch} --aggregate
python3 -B {zone}/sceneServer/tests/benchmark_report.py --batch {batch}
```

配置生成、隔离数据库初始化和批量种子数据由 benchmark_matrix.py 的 run_case/seed 执行；
实际生成配置逐案例归档。`--case-json` 可指定一个完整案例对象，输出到全新 batch，不能覆盖正式案例。
中断由 SIGINT/SIGTERM 收尾直接子进程；断电/SIGKILL 不保证收尾。
`--resume` 跳过已有 result.json（包括失败），拒绝覆盖没有 result.json 的中断目录，须显式保留后更名；
二进制哈希变化拒绝续跑。summary 由原始数据首次合并后缓存到 case-summary.json；原始数据变更时须
使用全新聚合目录或显式删除这些已知缓存后再聚合，不允许静默使用旧缓存冒充重算。

## 清理状态

归档校验完成后仅清理本批显式列出的工作目录、专用构建和临时日志；不清理整个 build-acceptance。
最终清单与核验见本报告末尾；历史产物、用户文件及系统MariaDB均保留。

## 已完成案例

|案例|通过|发送/目标 RPS|成功摊销 RPS|p50/p90/p99 µs|错误/未解决|守恒|
|---|---|---|---|---|---|---|
'''
    for r in rows:
        text += f"|{r['id']}|{r['passed']}|{r['actualSendRps']:.1f}/{r['targetRps']}|{r['successfulRps']:.1f}|{r['p50Us']}/{r['p90Us']}/{r['p99Us']}|{r['errors']}/{r['unresolved']}|{r['accountingValid']}|\n"
    text += '\n## 逐案例详细结果\n\n'
    def fmt(value):
        return '不可用' if value is None else f'{value:.2f}'
    for r in rows:
        work = evidence / r['id']
        result = json.loads((work / 'result.json').read_text())
        clients = [(p.stem, json.loads(p.read_text())) for p in sorted(work.glob('load-gw*.json'))]
        players = json.loads((work / 'players.json').read_text()) if (work / 'players.json').exists() else []
        placements = collections.Counter((p['gateway'], p['sceneId']) for p in players)
        text += f"### {r['id']}\n\n"
        text += f"- 发送目标达成率：{100*r['targetAttainment']:.2f}%；请求成功率：{100*r['successRate']:.4f}%。\n"
        text += f"- 时窗成功 RPS：{r['loadWindowSuccessRps']:.2f}；成功样本数：{r['latencySamples']}。\n"
        text += f"- 调度延误均值/最大：{r['meanScheduleDelayUs']:.2f}/{r['maxScheduleDelayUs']} µs。\n"
        text += f"- 网络命名空间 lo RX 平均：{fmt(r.get('namespaceLoRxMeanBytesSec'))} bytes/s（不归因到本案例进程；零值仅表示该接口计数未变化）。\n"
        text += f"- 在线保存证据：`{json.dumps(result.get('onlineSaveEvidence'), ensure_ascii=False)}`。\n"
        text += f"- Scene 退出：`{json.dumps(result.get('shutdown') or {k:v for k,v in result.get('cleanupExitCodes', {}).items() if k.startswith('scene')}, ensure_ascii=False)}`；错误：`{result.get('error', '无')}`。\n\n"
        text += '|网关|连接|Scene10/20 玩家|attempted|sent|成功|completed|unresolved|错误|Logout成功/失败|\n|---|---:|---|---:|---:|---:|---:|---:|---:|---|\n'
        for name, client in clients:
            gateway = int(name.removeprefix('load-gw'))
            text += f"|{gateway}|{client['connections']}|{placements[gateway,10]}/{placements[gateway,20]}|{client['load_attempted']}|{client['sent']}|{client['business_succeeded']}|{client['completed']}|{client['unresolved']}|{client['errors']}|{client['logout_succeeded']}/{client['logout_failed']}|\n"
            if client.get('business_errors'):
                text += f"\n网关 {gateway} 业务错误分类：`{json.dumps(client['business_errors'], ensure_ascii=False)}`。\n\n"
        resourcepath = work / 'resource-summary.json'
        if resourcepath.exists():
            resources = json.loads(resourcepath.read_text())
            text += '\n|进程角色|样本数|CPU均值/峰值 %|RSS均值/峰值 MiB|PSS峰值 MiB|读/写均值 bytes/s|\n|---|---:|---|---|---|---|\n'
            for name, value in sorted(resources.items()):
                pss = value['pssPeakBytes']
                text += f"|{name}|{value['samples']}|{fmt(value['cpuMeanPct'])}/{fmt(value['cpuPeakPct'])}|{value['rssMeanBytes']/1048576:.2f}/{value['rssPeakBytes']/1048576:.2f}|{fmt(pss/1048576 if pss is not None else None)}|{fmt(value['readMeanBytesSec'])}/{fmt(value['writeMeanBytesSec'])}|\n"
        proofpath = work / 'persistence-verification.json'
        text += '\n持久化最终校验：' + (f"`{proofpath.read_text().strip()}`" if proofpath.exists() else '**未完成，不能判定持久化正确**') + '。\n\n'
        metrics = json.loads((work / 'metrics-summary.json').read_text())
        text += '|指标来源|指标|首值|末值|采样峰值|\n|---|---|---:|---:|---:|\n'
        for name, values in sorted(metrics.items()):
            for key, value in sorted(values.items()):
                if (key.startswith('gateway_') and any(k in key for k in ('Dropped','Rejected','Errors','timedOut','pendingRoutes','backendReady'))) or (name == 'metrics-gw1.jsonl' and key in ('scene0_workerQueueDepth','scene1_workerQueueDepth','scene0_netQueue','scene1_netQueue')):
                    text += f"|{name}|{key}|{value['first']}|{value['last']}|{value['peak']}|\n"
        warnings = collections.Counter()
        for logfile in work.rglob('*.log.gz'):
            with gzip.open(logfile, 'rt', errors='replace') as source:
                for line in source:
                    for marker in ('queue full', 'queue_full', 'queue is full', 'worker queue', 'frame rate', 'frame limit', 'send buffer', 'overflow', 'shutdown persistence drain incomplete'):
                        if marker in line.lower():
                            warnings[marker] += 1
        text += f"\n日志关键字计数（匹配行数，不等同于独立事件数）：`{json.dumps(warnings, ensure_ascii=False)}`；完整压缩日志保留。\n\n"
    report = ROOT / 'docs/阶梯测试数据V1.md'
    report.parent.mkdir(exist_ok=True)
    report.write_text(text)
    print(f'{report}: {len(rows)}/{expected_count}, passed={passed}, complete={complete}', flush=True)


if __name__ == '__main__':
    main()