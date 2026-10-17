# Scene 监测模块

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
线程数减少不是性能改善证据；完整重构前后 CPU/内存/吞吐/p99 对照仍需单独执行。

## 验证入口

通过现有 scene_tests/CTest 和 Python unittest 验证。进程测试默认跳过，显式设置绝对路径 `SCENE_MONITORING_BINARY` 后运行 `test_monitoring_process`：
覆盖诊断开关不增加线程、最终输出、无效路径正常失败；不启用数据库。