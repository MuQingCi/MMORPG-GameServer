# busy 独立取证（2026-10-19）

> 同步日期：2026-10-19。本文按当前工作区维护，不代表已提交版本；`docs/*`、`LoginServer/` 和指定构建目录受根 `.gitignore` 忽略。本地证据链接不保证随仓库检出提供，历史测试不等于本轮重跑。

`SCENE_BUSY_DIAGNOSTICS=1` 独立于完整诊断开关；每秒向 metricsPath 加
`.busy.jsonl` 后缀的文件输出拒绝事件、累计分类计数、累计丢失数及 Worker Linux TID。
每次采集最多保留 4096 个事件，拒绝时才锁定记录器；成功请求不新增事件或时钟读取。
reason 0/1/2 为 direct/task-limit/byte-limit。事件携带内部 seq、玩家、客户端绑定及
pending 状态；同一 Net 所有者在 submit 返回后读取状态，没有跨字段采样竞争。
完整诊断仍有原有跨字段近似语义。客户端在此开关下仅输出前 4096 个 busy 的
player/requestId/接收单调时间/是否正式请求；超过上限显式输出丢失标志。
内部 seq 与客户端 requestId 不同，须通过 Gateway 现有 `relay b->c` 日志关联，
不能直接将两者比较；没有日志或事件丢失时不得宣称逐请求完整对账。
此探针不是性能修复，不改变队列或 ACK 语义，也不是零开销。

当前策略由 Scene YAML 的 `SceneServer.db.activeIndex` 在 Worker 构造时确定：
`true` 为活跃索引，`false` 回退完整扫描，不支持热切换。结构缺省值仍为 `false`，
仓库当前 YAML 显式设置为 `true`。旧环境变量 `SCENE_EXPERIMENT_ACTIVE_DB_INDEX`
不再控制 Worker；历史报告的环境变量仅适用于当时冻结二进制。
启用时保留完整任务身份表，仅遍历需要投递、等结果或待 ACK 的活跃索引；
完成且 ACK 成功的历史退出调度，重复结果可重新激活 ACK。任务间遍历顺序可能改变。
启动日志输出 `active-index` / `legacy-scan`。内部构造参数为
`PendingDbTasks::SchedulingPolicy`；兼容方法 `enableActiveIndex()` 在已有任务时返回
`false`，不静默切换。所有方法及 Sender 必须在所属线程调用，Sender 不得重入修改同一实例。
保留完整历史、60秒回收窗口、ACK背压及重复结果重新激活语义；失败任务移出索引但保留身份
和原 `unsettled()` 语义。已有热点成本改善证据，不等于 busy 根因或上线验收闭环。

# Scene 监测模块

## 性能证据入口（2026-10-19）

- [压测证据索引](../../../../docs/benchmark-results/README.md)分别记录活跃索引维护成本、客户端等待优化与短时万级吞吐，不能互相冒领收益。链接相对本文件；仓库文件路径和命令以仓库根目录为基准。
- 活跃索引 256 连接归档：成功 RPS 5,119.17 → 5,119.88，供给封顶；两 Scene 平均 CPU 约 4.02 → 2.48 核，后段维护 Self 合计 63.70% → 1.34%。微基准约 -99.92% 是固定任务规模的维护 CPU，不是业务吞吐增幅。
- 客户端优化最新 512 连接、30 秒轮成功 10,228.37 RPS，仍有最大约 5.22 秒调度滞后。该轮 perf 收尾 exit=-15，不能与旧轮占比作完整窗口对照；客户端 CPU 采用独立资源时间线量化。
- 当前策略与回退见本文开头 YAML 说明；历史实验报告的环境变量和“默认关闭”仅描述当时二进制，不覆盖当前仓库 YAML。

## 职责与所有权

- `SceneMetrics` / `WorkerMetrics`：业务线程记录轻量原子指标，所有者仍是 `SceneServer`。
- `QueueDiagnostics`：可选详细统计，所有者仍是 `MsgBus`；不执行文件 I/O。
- `MetricsSampler`：计算一秒窗口、速率、健康和过载；只能由监测服务消费窗口。
- `SceneMonitoringService`：借用以上对象，管理一个周期线程、缓存快照、导出和粘滞错误状态。
- `queueDiagnosticsJson`：只序列化已采集快照，重复序列化不消费事件。

## 调度

常规采样约 1s；`SCENE_QUEUE_DIAGNOSTICS=1` 时额外约 10ms 深度采样，每秒收集完整累计统计和取走事件。
关闭诊断时没有 10ms 唤醒，也不额外创建诊断线程。截止时间按 steady_clock 推进，错过周期只记 missed，不补造历史样本。
轻量诊断不调用 MetricsSampler，不消费极值窗口。stats 只读取缓存。

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

## 停机

先 stopPeriodic 并 join，按原业务顺序停止网络、排空 worker、停止 timer/DB，再 finish 最终采集并关闭输出。
排空过程中无周期采样，最终快照不代表连续观测或持久化成功。
finish 幂等；析构仅停止周期线程，不擅自假定业务已经停止。

## 成本与边界

仍为同步写盘，慢磁盘会推迟共享线程的任务，不保证硬实时 10ms。
事件每通道上限 4096，超出记录 droppedEvents；队列深度为近似值，等待直方图不包含未出队消息。
没有新增长链路 tracing、DB 控制队列诊断或无锁优化。
详细快照新增work字段：固定6类消息（business/db_result/service/timer/lifecycle/other）的
入队成功/拒绝、首次拒绝单调时间、handler耗时直方图，以及4类维护阶段耗时。
拒绝统计只含实际调用worker入队的尝试；网络层因生命周期pending而直接丢弃的请求不在此计数中。
阶段顺序为load/drain/reload、flush、AOI/recovery、persistence/retry；耗时为墙钟而非CPU。
统计有界、无逐请求事件；原子字段不是全局一致快照，采样跨字段存在并发偏差。
直方图沿用原等待桶，仅能给分位所在桶；计数微秒精度，亚微秒耗时可能记0。
诊断关闭不分配WorkDiagnostics；只保留已有开关判断，不新增采样或输出。
线程数减少不是性能改善证据；完整重构前后 CPU/内存/吞吐/p99 对照仍需单独执行。

### DB 维护诊断（可选）

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

## 验证入口

通过现有 scene_tests/CTest 和 Python unittest 验证。进程测试默认跳过，显式设置绝对路径 `SCENE_MONITORING_BINARY` 后运行 `test_monitoring_process`：
覆盖诊断开关不增加线程、最终输出、无效路径正常失败；不启用数据库。
