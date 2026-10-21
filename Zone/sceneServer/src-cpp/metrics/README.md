# Scene 监测模块

## 目录

- [模块简介](#模块简介)
- [职责与所有权](#职责与所有权)
- [调度](#调度)
- [输出及错误](#输出及错误)
- [busy 独立取证](#busy-独立取证)
- [DB 维护诊断](#db-维护诊断)
- [活跃任务调度](#活跃任务调度)
- [停机](#停机)
- [成本与边界](#成本与边界)
- [验证入口](#验证入口)
- [历史测试数据](#历史测试数据)

## 模块简介

监测模块采集 Scene 和 worker 指标，输出常规 JSON，也可以按需记录队列、busy 和 DB 维护诊断。业务线程记录指标，监测线程负责采样和文件输出。

## 职责与所有权

业务对象保留指标的所有权，监测服务借用它们采样。只有监测服务可以消费采样窗口。

- `SceneMetrics` / `WorkerMetrics`：业务线程记录轻量原子指标，所有者仍是 `SceneServer`。
- `QueueDiagnostics`：可选详细统计，所有者仍是 `MsgBus`；不执行文件 I/O。
- `MetricsSampler`：计算一秒窗口、速率、健康和过载；只能由监测服务消费窗口。
- `SceneMonitoringService`：借用以上对象，管理一个周期线程、缓存快照、导出和粘滞错误状态。
- `queueDiagnosticsJson`：只序列化已采集快照，重复序列化不消费事件。

## 调度

常规采样约每秒一次。开启 `SCENE_QUEUE_DIAGNOSTICS=1` 后，额外约每 10ms 采样队列深度，每秒收集累计统计并取走事件。关闭诊断后没有 10ms 唤醒，也不会额外创建诊断线程。

截止时间按 steady_clock 推进，错过周期只记录 missed，不补历史样本。轻量诊断不调用 MetricsSampler，也不消费极值窗口；stats 只读缓存。

`sampleDue()` 是确定性调度测试入口，只能由生命周期所有者在后台线程不运行时调用。
服务生命周期由一个所有者串行调用，snapshot/status 可供其他线程读取。记录器不得在服务存活期间重建或替换。

## 输出及错误

常规最新 JSON 与 `.queues.jsonl` 保持原有格式；常规 JSON 追加 `monitor_*` 状态：
分别标记 metrics/diagnostic 的写入与采样失败，并记录 missed、最大延迟/间隔和任务耗时。
最大任务耗时字段在任务结束后更新，因此同次常规输出不包含该次仍在执行的任务耗时。
诊断流为原始观测时间序列，实际采样间隔（含最后停机空档）应从 monotonicUs 计算；状态中的 gap/missed 仅统计周期采样。

错误状态粘滞，每种错误首次发生时记录一次日志。常规写入可在后续周期恢复，失败历史不清零；诊断写入失败后停止继续消费事件，常规采样仍运行。
常规输出失败不改变原先业务排空验收语义；诊断失败仍使 shutdownSucceeded 为 false。
显式诊断但路径无效时启动失败；不设置路径的普通 metrics 仍提供内存快照。

## busy 独立取证

设置 `SCENE_BUSY_DIAGNOSTICS=1` 可以单独记录 busy，不必开启完整诊断。每秒输出到 metricsPath 加 `.busy.jsonl` 后缀的文件，内容包括拒绝事件、分类累计计数、丢失数和 Worker Linux TID。

- 每次采集最多保留 4096 个事件。只有拒绝请求时才锁定记录器，成功请求不额外记事件或读时钟。
- reason 0/1/2 分别为 direct/task-limit/byte-limit。事件包含内部 seq、玩家、客户端绑定和 pending 状态。
- 状态由同一 Net 所有者在 submit 返回后读取，没有跨字段采样竞争；完整诊断的跨字段数据仍是近似快照。
- 客户端只记录前 4096 个 busy 的 player/requestId/接收单调时间/是否正式请求，超过上限会标记丢失。

内部 seq 与客户端 requestId 不能直接比较，需要通过 Gateway 的 `relay b->c` 日志关联。日志缺失或事件丢失时，无法逐请求完整核对。这个探针有开销，只用于诊断，不改变队列或 ACK 规则。

## DB 维护诊断

`work[].phases` 保留前4项含义，追加 4=AOI tick、5=AOI恢复、
6=PlayerPersistence tick、7=PendingDbTasks tick、8=tickControls、9=诊断状态扫描。
4/5 是阶段2的子项，6/7/8 是阶段3的子项，不能重复相加；耗时包含计量开销。
`dbTasks` 每秒由所属 Worker 扫描一次：总数、等待投递、等待结果、完成待ACK、
完成历史、投递失败。`dbSampleUs` 为该次状态采样开始的单调微秒时间。
`dbScanned` 累计记录两个原有完整扫描各自访问的条目数（调用前任务表大小）；
不包含状态采样、submit容量回收和unsettled扫描。
`netPending` 由 Net 线程按秒发布：直接busy、条数busy、字节busy、expired、
业务pending条数/字节、生命周期pending条数、stale、internalErrors、accepted。
同次同时超过条数和字节时仅归为条数，与原准入短路顺序一致。
事件 `net_send_buffer_bytes` / `net_send_buffer_max_bytes` 按秒采样当前连接发送缓冲
总量/最大值，不代表采样间隔内峰值。Net队列仍使用顶层 `netQueue`；
DB结果回投使用 `work[].classes[1]` 的入队与拒绝统计。
新增状态采样有独立phase9开销；既有详细诊断、计时器和原子计数的其他开销未被扣除。
关闭诊断不调用任务状态扫描；没有修改调度频率、ACK、去重、保留策略或队列容量。

`work[].dbProbe` 为后续细分归因：两个数组对应tick/tickControls，每1024轮取1轮，
各字段累计为：采样轮数、访问PendingDispatch/AwaitResult/完成待ACK/完成历史/失败
的条目数、发送回调次数、调用墙钟ns、调用线程CPU ns、回调墙钟ns、回调线程CPU ns、
CPU时钟读取失败次数。visited为访问当时状态，不同于函数结束后状态。
采样函数内部只做本地条目计数，不逐条读时钟；回调单独计时，包含在调用总耗时内。
CPU时钟使用CLOCK_THREAD_CPUTIME_ID；wall与CPU时间区间略有偏差且都包含计量开销。
不能简单将采样值乘1024作为精确全量开销：周期采样可能偏向特定工作/截止时间。
非采样路径不读CPU时钟、不计visited；函数保留相同状态机与发送顺序。

## 活跃任务调度

活跃索引设置：

| 配置 | 作用 |
| --- | --- |
| `SceneServer.db.activeIndex` 为 `true` | 只遍历需要投递、等待结果或待 ACK 的任务 |
| `SceneServer.db.activeIndex` 为 `false` | 回退完整扫描 |

策略在 Worker 构造时确定，不能热切换。结构缺省为 `false`，仓库 YAML 显式设为 `true`。旧环境变量 `SCENE_EXPERIMENT_ACTIVE_DB_INDEX` 已不起作用，历史报告里的用法只适用于当时二进制。

活跃索引保留完整身份表和 60 秒回收窗口。完成且 ACK 成功的任务退出调度，重复结果可以重新激活 ACK；失败任务退出索引，但保留身份和原 `unsettled()` 含义。ACK 背压规则不变，任务遍历顺序可能变化。

启动日志记录 `active-index` / `legacy-scan`。内部构造参数是 `PendingDbTasks::SchedulingPolicy`，已有任务时调用 `enableActiveIndex()` 返回 `false`。所有方法和 Sender 都必须在所属线程调用，Sender 不得重入修改同一实例。

历史测试表明维护热点成本下降，但还没有据此确定 busy 的根因，也不代表上线验收通过。

## 停机

1. 调用 stopPeriodic 并 join，等待周期线程结束。
2. 按业务顺序停止网络、排空 worker、停止 timer/DB。
3. 调用 finish 完成最后一次采集，关闭输出。

排空期间没有周期采样，最终快照不能替代连续观测，也不能证明持久化成功。finish 可重复调用；析构只停止周期线程，不负责判断业务是否已停止。

## 成本与边界

文件输出使用同步写入，慢磁盘会推迟同一线程的后续任务，10ms 采样不是硬实时保证。事件每通道最多 4096 个，超出后累计到 droppedEvents；队列深度是近似值，等待直方图也不包含未出队消息。

详细快照的 work 字段按 business/db_result/service/timer/lifecycle/other 六类消息记录入队成功、拒绝、首次拒绝单调时间和 handler 耗时直方图。拒绝计数只包含实际尝试入 worker 队列的消息，网络层因生命周期 pending 而直接丢弃的请求不在其中。

四个维护阶段依次是 load/drain/reload、flush、AOI/recovery、persistence/retry，耗时使用墙钟时间，不是 CPU 时间。直方图沿用等待桶，只能确定分位数所在区间；微秒精度可能把亚微秒耗时记为 0。

统计数量有上限，不记录逐请求事件。原子字段不构成全局一致快照，读取不同字段时可能存在并发偏差。关闭诊断后不分配 WorkDiagnostics，也不新增采样或输出，只保留开关判断。

本模块没有完整请求追踪、DB 控制队列诊断或无锁优化。线程数减少不能单独证明性能改善，重构前后的 CPU、内存、吞吐和 p99 仍需另行测量。

## 验证入口

本模块随 Scene 构建，先按[场景服构建说明](../../README.md#构建与运行)生成可执行文件。

通过现有 scene_tests/CTest 和 Python unittest 验证。进程测试默认跳过，可在仓库根目录执行：

```bash
SCENE_MONITORING_BINARY="$(realpath out/zone/bin/sceneServer)" \
  python3 -B -m unittest discover -s Zone/sceneServer/tests -p 'test_monitoring_process.py'
```

示例中的二进制路径相对仓库根目录，请按实际构建目录调整。测试会切换到临时工作目录，所以用 `realpath` 在启动前解析路径，不依赖固定的项目安装位置。

检查内容包括诊断开关不增加线程、最终输出、无效路径正常失败；不启用数据库。

## 历史测试数据

| 历史测试 | 结果和限制 |
| --- | --- |
| 活跃索引，256 连接 | 成功 RPS 5,119.17 → 5,119.88，供给已到上限；两 Scene 平均 CPU 约 4.02 → 2.48 核，后段维护 Self 合计 63.70% → 1.34% |
| 固定任务规模微基准 | 维护 CPU 约 -99.92%，不是业务吞吐增幅 |
| 旧客户端等待优化，512 连接、30 秒 | 成功 10,228.37 RPS，最大发送调度滞后仍约 5.22 秒；perf 收尾 exit=-15，CPU 数字取自独立资源时间线，不能用 perf 占比做完整窗口对比 |

这些测试来自历史版本，用于区分维护成本与端到端吞吐；不是本次重构的测量结果。当前调度策略见上文。
