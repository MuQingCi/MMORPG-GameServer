# 2026-10-17：取消队列早停后的阶梯、边界与恢复

本节为新增实验，不覆盖先前结果。仅改造压测工具，未调整业务代码、后端帧率保护或系统 MariaDB；未提交工作区。

## 执行语义

- 删除连续非零队列、指标陈旧和 backendReady 数值导致的负载早停；它们保留为采集和事后评价数据。
- 单档错误、低吞吐或 p99 不达标不再跳过控制组和更高档。服务进程退出、准入超时、单档总执行期限仍是执行边界。
- 每侧入口上限 512；沿初始 x 至 x+32 往返（下界至少 1），避免长测越界。原无 DB `(0,0)` 回程的空 protobuf ACK 触发问题由压测移动路径避开，保留失败 smoke 证据。
- 新增成功写入 socket 的发送计数、累计/最大调度延迟、每秒业务成功数及离线报告；每连接单在途闭环机制未变，配置 RPS 不是实际到达率。
- 报告的 successfulRps 是成功 ACK 总数除以负载秒数，可能含排空期完成；`loadWindows` 根据真实单调时钟间隔另列负载窗口速率。socket 写入不等于服务端成功接收。

## 阶段 A：60s 负载 + 最多 30s 排空

7 档、14 次配对全部执行，没有在失败档停止升级。下表成功 RPS 为双 Gateway 合计，p99 为两客户端较大值，仅对成功响应计时，错误占优时不能据低 p99 宣称健康。

| 总连接 | 持久化成功 RPS | 无 DB 成功 RPS | 持久化/无 DB p99(ms) | 结果 |
|---:|---:|---:|---:|---|
|128|1275.74|1275.74|10.577 / 7.139|两者通过|
|192|26.49|35.67|11.836 / 17.446|两者失败|
|256|27.08|38.85|14.994 / 18.627|两者失败|
|384|25.60|18.75|35.332 / 20.457|两者失败|
|512|22.73|13.51|47.550 / 31.877|两者失败|
|768|0|0|不适用|准入/链路失效，无有效成功负载|
|1024|0|0|不适用|准入失败，无有效成功负载|

128 持久化的最终位置/版本、背包、两角色显式重登核验通过。Scene 单 worker 采样峰值队列 46，说明非零队列本身不等于失败。192 起日志明确记录 backend 连接 `frame rate exceeded (1001/s)`，然后关闭后端连接；保持现有 1000 帧/秒保护，没有人为调高。

## 阶段 B：中间档 160 总连接，120s，配对重复三次

| 重复 | 持久化成功 RPS | 无 DB 成功 RPS | 结果 |
|---:|---:|---:|---|
|1|12.79|15.98|两者失败|
|2|14.48|16.00|两者失败|
|3|13.17|16.55|两者失败|

三次均触发现有后端帧率限制。观测到的当前配置可用边界位于总连接 128（成功）与 160（失败）之间；不是精确阈值，128 尚未做三次长测。原方案在初始 128/192 间插入 160，本轮不继续二分。

## 阶段 C：192/256 总连接，120s + 30s 排空 + 30s 额外保留服务

| 总连接 | 持久化成功 RPS | 无 DB 成功 RPS | 恢复结果 |
|---:|---:|---:|---|
|192|13.07|17.94|四个 Gateway 观测均未恢复后端链路|
|256|11.62|19.21|四个 Gateway 观测均未恢复后端链路|

每次额外保留服务采得 31 条快照，最终两个 Gateway 的 backendReady 均为 0；负载结束后的排空采样也未满足链路恢复条件。不能把空队列称为恢复成功。失败档未完成最终 SQL/背包/重登全量核验，完整性结论为未验证，而非通过；退出时 Scene 干净结束不等于客户端快照一致性已验证。

## 结论与限制

当前测得的是**现有配置的后端帧率保护边界**，并非 Scene CPU、持久化或成功吞吐的物理饱和边界。解除这一配置瓶颈后的饱和边界仍未知。无 DB 同样失败，不支持把本次退化归因于数据库。

工具统计中 unknown response、控制错误、logout 失败可能额外计入 completed/errors，失败档不能用 completed 当成功吞吐；报告使用 business_succeeded。客户端最多每连接一个在途，调度延迟也可能来自服务响应反压，不能仅凭延迟断定发压端饱和。同机客户端与服务竞争资源，尚未排除发压端限制。恢复观测不是成功新请求探针，不证明服务重新可用。

所有路径以下列绝对根目录为前缀：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/`。

- A：`persistence-staircase-1792208311770011905/`
- B1/B2/B3：`persistence-staircase-1792209411500757902/`、`persistence-staircase-1792209719305943366/`、`persistence-staircase-1792210026404650801/`
- C：`persistence-staircase-1792210333936618395/`
- 各目录均有 `summary.json`、各档 `result.json`、`boundary-report.json`，以及原始客户端/逐 worker 指标/资源/恢复日志。
- 历史失败日志含无条件 PASS 文案；以非零退出码、JSON passed=false 和核验产物为准。本轮最终代码已将这类文案改成条件输出，原始证据不篡改。

# 隔离持久化验收

本工具仅供显式执行，不默认加入 CTest，也不使用区服配置中的数据库。
需要本机 MariaDB 初始化工具、服务端、客户端，以及 Redis 服务端。

```bash
cmake -S /home/lanxiyuan/Project_Cpp/GameServer/Zone \
  -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance
cmake --build /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance \
  --target sceneServer gatewayServer persistence_dao_probe -j2
python3 /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_e2e.py \
  /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance
```

工具使用 `/tmp/clearmoon-persistence-e2e-*` 独立目录、回环端口、数据库
`clearmoon_persistence_e2e` 和专用 DML 账号。不会访问系统现有数据库。
配置凭据使用随机密码和受限目录；退出时停止本次进程并删除临时数据。
成功摘要保存在构建目录的 `persistence_e2e_summary.txt`，不包含凭据。
失败时输出经密码遮蔽的服务日志。SIGKILL/主机故障无法执行 Python 清理，
这种情况下应先确认残留进程属于本次临时目录，再手工清理，不能停止其他实例。

## 验收内容

表结构来源为仓库根目录的 `sql/001_player.sql` 和 `sql/002_inventory.sql`，
不是 `Zone/sceneServer/sql/`。脚本在临时数据库中自动导入，无需手工执行。

- 真驱动一致性加载，非零数据库出生点和非空背包。
- 外键失败时玩家、背包和版本整体回滚。
- 相同版本载荷重复保存，旧版本拒绝。
- 网关鉴权、进场、移动、主动离线、立即重新绑定和加载。
- 在线周期保存；修改后关服排空；进程重启恢复。
- 复用网关连接的另一玩家 TCP 断开后独立保存。
- SQL 查询核对位置、版本推进及物品内容；日志检查关服未完成错误。

快速重登测试覆盖真实正常时序，但不保证人为延迟保存的每个交错都被触发；
该交错还需对应单元测试。工具不验证持久任务日志、进程崩溃不丢失、死锁、
COMMIT 断链结果未知恢复、跨网关接管或客户端 AOI 视野推送。

回环端口动态选取，选取与启动之间仍可能存在竞争；端口被占用会使测试失败。
MariaDB 是当前机器上的验收后端，不应将结果当作所有 MySQL 版本兼容证明。

## 双 Gateway 持久化准入与负例

```bash
cmake --build /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance \
  --target sceneServer gatewayServer gateway_load_client -j4
python3 -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_load_e2e.py \
  /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance
python3 -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_load_e2e.py \
  --negative-save /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance
```

每次运行独立初始化临时 MariaDB/Redis，导入根目录 SQL，创建随机密码的
`persistence_test` DML 账号。固定每 Gateway 8 个有效角色，每连接 10 RPS，
负载 10 秒、排空 5 秒；不用于容量上限判断。持久化模式显式固定 Gateway 1
到 Scene 10、Gateway 2 到 Scene 20，避免依赖无序注册表的实例遍历顺序。
默认无 DB 脚本仍使用原路由选择，不覆盖跨 Gateway 角色接管。

- 角色初始位置非零、保存版本为 7、背包中有 9 个测试物品。
- 在线负载第 2～8 秒查询全部角色版本推进，脱敏摘要保留首次证据时间。
- 客户端仅在成功登出后导出确认位置及成功移动数；与最终 SQL 快照逐角色核对。
- 核对位置、Scene、版本及物品；每个 Gateway/Scene 路径至少一个角色重登恢复。
- 缺失角色、Scene 不匹配均要求返回加载失败 RetTip 1010，且不修改数据库。
- 正常模式要求两个 Scene 在 20 秒内成功退出且无未排空日志。
- `--negative-save` 在正常核对后，仅对临时库增加拒绝 UPDATE 的触发器。
  要求已确认移动不能错误推进数据库，Scene 10 有界退出码为 1、Scene 20 为 0，
  并出现明确的未排空日志。负例通过不代表丢失的修改可以自动恢复。

脱敏证据位于
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-load-*/`，
包括客户端汇总、逐玩家 JSONL、指标、服务日志和数据库校验摘要；不复制配置、
凭据、临时库数据目录。`summary.json` 区分正常/负例并记录在线推进证据。
保存失败后的 Scene 进程现在返回非零退出码，而不是仅写错误日志后返回成功。

边界：双 Gateway 测试不覆盖跨网关接管、异常 COMMIT 结果恢复、崩溃不丢失，
也不据此解释此前 256 连接过载。重启恢复另由单 Gateway 验收覆盖。

### 2026-10-16 本地验证记录

- 正常模式证据：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-load-1792158078665673931/`。
- 保存失败负例：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-load-1792158008528883215/`。
- 两次均每侧 784 次移动成功、业务/解码错误为 0，16 个角色在线版本推进，
  最终位置/物品核对、两条 Gateway/Scene 路径重登与加载负例通过。
- 新构建下单 Gateway 持久化及默认无 DB 双 Gateway 回归均通过；后者每侧 224 次移动。
- Python 配置/入口测试 13 项通过；客户端行为测试 3 项通过（含正 drain 最终采样回归）。
- 全量 CTest 6/8：`gateway_e2e`（模拟后端移动读取）和 `gateway_chat_global`
  （accepted 应答断言）失败，尚未确定根因；不能宣称全库测试通过。

### 阶梯测试入口

上述两项失败后续已定位：`gateway_e2e_client`、`chatServer`、`globalServer`
为旧构建产物，重建全部目标后全量 CTest 为 8/8；无需修改业务路由。

单档仍兼容旧命令，默认每侧 8 连接、10 RPS、10 秒负载、5 秒排空；也可显式指定：

```bash
python3 -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_load_e2e.py \
  /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance \
  --connections 16 --rps 10 --duration 30 --drain 10 --queue-seconds 3
python3 -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_staircase.py \
  /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance
```

阶梯默认每侧 8→16→32→64，固定 10 RPS、30 秒负载、10 秒排空。
每档依次运行持久化和无 DB 对照；任一失败立即停止，不启动更高档。
`--stages 8,16` 可缩小范围；禁止超过每侧 64、非递增或重复档位。
造数位置沿用 x=5+i、y=6（最多 x=68），不会触及地图障碍；同时校验
RPS×时长的移动预算，避免持续向右移动越过 x=2000 边界。

每档重新初始化隔离库/进程。持久化仍核对全部玩家最终坐标、Scene、版本和物品。
停止门槛包括持续 3 个秒级非空队列样本、后端缺失/指标失效、客户端业务/解码错误、
超时、未排空、数据库不一致；阶梯另外要求实际完成数达到目标的 90%，p99≤100ms。
这些是诊断门槛，不是生产 SLA；非空队列连续样本不等同于队列严格单调增长。

两种模式均固定 Gateway 1→Scene 10、Gateway 2→Scene 20，但无 DB 模式从默认原点
开始，持久化从非零数据库位置开始；属于路径对照而非完全相同世界状态。
角色为密集条带分布，不代表分散玩家或生产 AOI 场景。
持久化 flush 固定 1 秒，用于保存推进证据，也不等同于默认 30 秒的生产配置。

阶梯证据位于
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-*/summary.json`。
每档输出目标配置、实际完成吞吐、p95/p99、错误及对应证据目录。
`resources.jsonl` 从 Linux `/proc` 每秒采样子进程树的 PID、启动时间、累计 CPU tick、
RSS 和时钟频率，可区分客户端、Gateway、Scene、MariaDB 的资源占用；不含命令行凭据。
该资源采样包含准备和收尾阶段，分析稳态 CPU 时需使用相邻样本差分并对齐负载时间。
同机压测、单次短时运行只能证明已测档位，不能当作最大容量或生产数据库性能结论。

#### 首轮阶梯结果（2026-10-16）

证据：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-1792158788584252003/summary.json`。
四档两种模式全部通过，实际完成吞吐约为目标的 99.3%，错误和未完成均为 0。

| 每侧连接 | 总连接 | 单模式总完成数 | 持久化最差 p99 | 无 DB 最差 p99 |
|---:|---:|---:|---:|---:|
| 8 | 16 | 4768 | 2.295ms | 1.995ms |
| 16 | 32 | 9536 | 4.064ms | 2.809ms |
| 32 | 64 | 19072 | 8.283ms | 4.326ms |
| 64 | 128 | 38144 | 19.703ms | 8.927ms |

持久化全部档位的在线版本推进、最终坐标及物品核对通过；每档资源 JSONL 至少
41 个样本。尚未触及停止门槛，不能认定瓶颈或最大容量。持久化尾延迟随档位上升，
下一步应在已测高档重复/延长观察，并结合 CPU、队列与 DB 指标定位，而非直接翻倍。

最终接口小档回归证据：
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-1792159259332595822/`。
此前一次小档在 Gateway bind 阶段因随机端口冲突失败，证据保留于
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-1792159217526471173/`；
该次按预期第一档即停止，未升级。端口选择和 bind 之间存在竞争，当前不自动重试。

#### 最高档重复测试：保护触发，未进入长时阶段

本轮构建和预检通过（CTest 8/8、Python 18 项）。计划串行执行三次每侧 64
连接的 30 秒对照，然后在稳定时进入 120 秒；实际第一次持久化运行即失败停止。
证据：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-1792159911505107405/`。

- 第 3 秒触发队列保护：两 Scene 在第 1/2/3 秒的 worker 队列合计分别为
  16/46/81 和 24/44/124，连续三个非空样本，后端就绪数为 4。
- 每侧最终发送并完成 1920 次请求；业务/解码错误、超时、未完成均为 0。
  保护导致客户端停止，未执行正常登出（logout_succeeded=0），不能视为正常准入通过。
- 负载第 2030ms 已确认全部 128 角色版本超过初始版本 7。
- 因提前失败未运行最终逐玩家 SQL 对照/重登，不能宣称最终坐标和背包验收通过。
- 最终 Scene 指标 playerCount、workerQueueDepth、netQueue 均为 0；未发现保存阻塞、
  保存失败或未排空日志。已核对无本轮隔离进程残留。
- 短窗口进程 CPU（100%=一个核）：Scene 各约 40～47%、Gateway 各约 19～22%、
  客户端各约 13～17%、MariaDB 约 12～15%。仅有数秒数据，不足以判断稳定内存趋势，
  也不能排除单线程或瞬时热点。

后续第二/第三次短测、无 DB 对照以及两次 120 秒均未启动，未放宽保护或增加并发。
上一轮同档曾通过，本轮不能把“短窗口队列增长”直接解释为数据库容量瓶颈。
下一步需先审查秒级采样与周期性发压/保存/AOI 是否存在采样相位影响，并补充
队列驻留时间或更细时间分辨率证据；若要重跑或改变停止策略，应单独说明实验条件。

#### 高频诊断（2026-10-16）：未复现保护触发

新增 `--queue-diagnostics`，默认关闭。开启后 Scene 独立线程以约 10ms 间隔读取
原始 worker/net 近似队列深度，写入各自 metrics 文件旁的 `.queues.jsonl`，
不调用或重置原 `sampleMetrics()` 窗口。记录实际 steady-clock 微秒时间；客户端
秒级 JSONL 新增 `readMonotonicUs`，可与 Scene 的 `sampledAtMs`/序号计算快照年龄。
`elapsedMs` 仍是计划采样点，不是实际读取时间。

worker 入队尝试前刷新进程内时间戳，出队时记录等待桶；重入及广播经过相同入口。
分类为 0=网络请求、1=DB 结果、2=service/AOI、3=其他。修复入队 business 口径：
现在与出队一样按 `MSGTYPE_CONN_DATA` 计数，不再排除带 client binding 的消息。
等待直方图是累计值，桶上界（微秒）为 100/500/1000/2000/5000/10000/20000/
50000/100000/无穷；不能将桶边界当作精确 p99，未出队消息不在直方图内。

记录实际发生的 AOI tick 起止和序号、保存扫描/提交阶段起止及 dirty 数，另记保存
完成/失败时间及 task id。`flush_submit` 时长不是异步数据库事务完成耗时。
当前尚未覆盖 net/DB 任务队列的驻留直方图、入队失败分类计数、完整逐 task 的
提交到完成关联、保存扫描总数及 AOI 产生消息数。不能据此宣称已完成全部观测计划。

离线聚合工具：

```bash
python3 -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/analyze_queue_diagnostics.py \
  /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-load-1792161269829229975/run/scene10.metrics.json.queues.jsonl
```

输出相邻 `.summary.json`，包含 100ms 窗口 min/max/avg、非空样本比例、最长连续
非空采样跨度和实际最大采样间隔。窗口均值及比例按样本计算，不是时间加权值；
队列深度是近似值，连续非空采样跨度不能证明样本之间队列从未清空。

小档观测开销检查（每侧 8、10 秒、5 秒 drain）：
- 关闭证据：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-load-1792161115013459137/`。
- 开启证据：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-load-1792161169112694236/`。
- 两次均通过，每侧 784 次移动、错误 0。两 Scene 全过程平均单核 CPU 约
  6.67%/6.86%（关）与 6.75%/6.62%（开）；最差 p99 从 3.975ms 到 6.214ms。
  单次配对且含准备/收尾，不足以证明无扰动或给出稳定开销比例。
- 开启时最大采样间隔约 97ms，名义 10ms 不代表硬实时保证。

原配置短诊断证据：
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-load-1792161269829229975/`。
每侧 64、10 RPS、30 秒负载、10 秒 drain，原连续三秒非空保护保持不变。

| 指标（负载期） | Scene 10 | Scene 20 |
|---|---:|---:|
| 高频样本数 | 2953 | 2954 |
| worker+net 合计为空的样本数 | 2539 | 2558 |
| 采样到的最大合计深度 | 208 | 193 |
| 最长连续非空采样跨度 | 25.658ms | 25.056ms |
| 最大采样间隔 | 17.841ms | 17.296ms |
| AOI tick 最大执行耗时 | 18.327ms | 20.834ms |
| flush_submit 最大执行耗时 | 3.789ms | 3.520ms |

- 两侧各完成 19072 次移动，共 38144；业务/传输错误为 0，各 64 个正常登出。
  客户端 p99 分别 20.403ms/18.611ms。
- 2005ms 时全部 128 角色版本推进；最终逐角色快照匹配、背包保留，两个代表角色
  重登成功，缺失角色和 Scene 不匹配负例通过，见该目录下
  `run/persistence-verification.json`。最终 playerCount、worker/net 队列均为 0。
- 全过程 worker 等待记录无缺失时间戳，事件无丢弃。网络请求平均等待约
  1.227ms/1.127ms，最大 33.721ms/21.702ms；DB 结果平均等待约
  0.162ms/0.112ms。这是结果回到 worker 后的排队，不是 DB 执行耗时。
- 本次秒级 Scene 快照年龄约 587～612ms；Scene 10 仅在第 13/23 秒读到非零
  worker 队列（36/18），Scene 20 全部为空，未触发原保护。
- 已检查无本轮隔离进程残留。随后全量重建和 CTest 8/8 通过。

结论：本次有明显短突发，但负载期约 86% 的高频样本为空，未复现此前失败。
不能将本次通过当作此前保护相位误报的因果证明，也不能排除观测改变调度。
尚未执行采样偏移、连接错峰、保存/AOI 初始偏移实验；不修改保护，不恢复 120 秒。

#### 低扰动观测续实现：小档通过，开销门槛未通过（2026-10-16）

当前诊断改为按 worker/net/MySQL/Redis shard 分锁，锁外序列化；近似队列深度
仍约 10ms 读取，直方图和有界事件约每秒导出，退出补全量快照。增加普通任务
队列接纳/失败计数和 net/DB 排队桶；控制队列目前不在这些计数内。
通道编号为 worker `[0,N)`、net `N`、MySQL `N+1`、Redis `N+2+shard`，
最后一项为非法目标/消息计数。接纳数组依次为尝试、成功、背压、非法目标、非法消息。
普通深度样本的 `waits` 为空，须用 `fullSnapshot` 识别直方图快照，不可视为空计数。

保存任务通过进程内 `diagnosticSaveTask` 标记区分，不使用 `SYS_FLUSH` 识别，
因为加载也使用该方法号。记录登记、DB 接纳、初次实际执行以及结果应用事件，
按单 Scene 文件中的 task id 关联。DB 接纳事件起点是入队尝试，执行时间不包括
连接恢复和解码；现有事务重试执行尚未完整展开计时，不能宣称覆盖所有重试耗时。

首次小档批次在第二次运行停止：业务验收通过，但加载被误分类为保存导致关联缺失。
失败证据保留：
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/phase-small-1792162674825254693/`。
修正后重新执行关→开→开→关，每侧 8、10 秒负载、5 秒 drain：
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/phase-small-1792162979806142099/`。

| 次序 | 诊断 | 两 Scene 负载区间单核 CPU | 两客户端 p99 |
|---|---|---|---|
| 1 | 关 | 7.70% / 7.80% | 3.849ms / 4.863ms |
| 2 | 开 | 9.72% / 9.61% | 8.038ms / 2.616ms |
| 3 | 开 | 8.77% / 8.56% | 2.764ms / 2.958ms |
| 4 | 关 | 7.50% / 7.71% | 1.700ms / 1.969ms |

四次每侧均完成 784 次、错误为 0；开启两次每个 Scene 各有 88 个完整保存任务关联，
无丢弃事件和缺失排队时间戳。全过程最大采样间隔约 83～100ms，包含收尾阶段；
分析对超过 30ms 的缺口中断连续观测跨度，不用缺口证明持续积压或队列清空。

开启诊断时 CPU 一致上升，超过本组关闭对照的波动；p99 不呈稳定同向变化。
不能据此精确量化生产开销，但不满足本轮“观测可靠后再进高档”的准入要求。
因此新的总计 128 连接、最多三次批次**未启动**；不得将旧高档结果当作新实现的验证。

新增 `/home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_phase_diagnostics.py`
固定原负载和原保护，最多三次、首次失败即停，记录二进制摘要；失败时尽可能保留
两 Scene 部分分析及验证缺口。当前仅通过驱动单测，未做高档端到端准入。
后续先优化或降低逐消息诊断开销，再复验开关对照；不放宽保护，不运行相位干预或长测。

#### 关闭高开销诊断的受控基线（2026-10-17）

本轮将容量基线验证与故障归因分开：诊断扰动门槛仍未通过，但不将其作为关闭诊断后
受控压测的阻塞。保留既有工作区、原连续队列保护及业务核验，不修改业务代码。
现有持久化入口硬限制每侧最多 64 连接，因此本轮只验证总计 128 连接，未绕过边界
扩容，也不宣称完成极限压测或测得生产容量。

执行前全量构建通过；持久化入口单测 8 项、指定正确客户端路径后的客户端/入口测试
13 项及 CTest 8/8 通过。首次直接运行客户端测试时，因未设置 `GATEWAY_LOAD_CLIENT`
导致 3 项路径断言失败，重建并设置该变量后通过，不是业务失败。

执行命令（清除继承的 `LOAD_*` 和 `SCENE_QUEUE_DIAGNOSTICS` 环境变量后）：

```bash
python3 -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_staircase.py \
  /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance \
  --stages 64 --rps 10 --duration 30 --drain 10 --queue-seconds 3
```

证据目录：
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-1792206256192878386/`。
构建产物 SHA256 和运行边界记录于
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/controlled-load-20261017-metadata.json`。

| 模式 | Gateway 1/2 各完成移动数 | Gateway 1/2 实际 RPS | Gateway 1/2 p99 |
|---|---:|---:|---:|
| 持久化 | 19072 / 19072 | 635.73 / 635.73 | 9.656ms / 8.594ms |
| 无 DB 对照 | 19072 / 19072 | 635.73 / 635.73 | 7.178ms / 8.584ms |

两种模式均通过，每侧正常登出 64 个角色，业务/解码错误、超时、未决请求和最终未排空
计数均为 0。持久化运行在负载开始后 2006ms 确认全部 128 个角色版本推进；最终逐角色
快照匹配、背包保留、两条路径代表角色重登及缺失角色/场景不匹配负例均通过。
未生成高开销队列诊断流，无提前停止、指标过期或强制结束标记；运行前后构建产物
SHA256 一致，结束后无本轮隔离服务残留。系统既有 MariaDB 未被停止。

结论：当前构建在本机此负载下完成一次持久化及配对无 DB 短测。两次串行运行不足以
精确量化持久化开销，也不能证明此前保护误报、观测无扰动或长时间稳定；未执行新的
带诊断高档批次、超过 128 连接的扩容、相位干预或长测。无独立审查代理，本轮为自查。

#### 扩容入口与首次失败即停批次（2026-10-17）

经批准，持久化入口的参数校验与角色生成上限统一扩展至每侧 128 连接（总计 256），
默认参数不变。仅修改测试入口及测试，不修改业务实现、地图或保护阈值。
新增测试先因旧上限拒绝 96 连接失败，再通过；覆盖每侧 64/96/128 的角色唯一性、
场景映射、初始版本及现有地图内 30 秒、10 RPS 的合法无障碍移动路径，并拒绝 129。
扩容阶梯首次失败即停也有回归覆盖。全量构建、场景 Python 测试 19 项、客户端/入口
Python 测试 13 项及 CTest 8/8 通过。

批次命令（同样清除继承的负载和队列诊断环境变量）：

```bash
python3 -B /home/lanxiyuan/Project_Cpp/GameServer/Zone/sceneServer/tests/persistence_staircase.py \
  /home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance \
  --stages 64,96,128 --rps 10 --duration 30 --drain 10 --queue-seconds 3
```

证据目录：
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-1792206776612037306/`。
边界、构建产物摘要及执行决定见
`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/expanded-load-20261017-ledger.json`。

本批次在首档总计 128 连接的持久化模式失败并返回 1；未执行该档无 DB 对照，也未启动
192/256 连接。原保护于 4000ms 触发：后端就绪数 4、连续三次采样工作队列非零，
当时最大单 Scene 工作队列合计 84。2000/3000/4000ms 两 Scene 队列分别为
1/2、18/39、18/84。采样有效，但离散非零样本不能证明两次采样之间队列从未清空，
不能据此断言持续积压、保护误报或项目容量上限。

停止前每侧完成 2496 次移动，错误、超时和未决请求均为 0，p99 为
23.878ms/24.321ms；2076ms 已确认全部 128 角色在线版本推进。客户端正常登出计数
均为 0，未进入最终 SQL 快照/背包/重登核验，因此不能把本次写成业务全链路验收通过。
结束快照两 Scene 的玩家数和工作/网络队列均为 0；已确认无本轮服务残留，系统原有
MariaDB 未被停止，运行前后服务及客户端二进制 SHA256 一致。

遵守首次失败即停，不自动重跑或放宽保护。此前同配置通过与本次失败并存，说明短测
结果尚不稳定；根因未确定，下一步应先解释保护触发及队列变化，再决定新的扩容批次。
无可用独立审查代理，本轮仅作自查，不宣称完成极限性能测试。
## 2026-10-17：网关帧阈值单变量对照（后续执行）

本轮只把隔离实例 gateway.maxFramesPerSec 从 1000 调到 100000，Scene 仍为 1000；没有关闭生产保护或修改业务实现。新增入口 `--frame-limit`，默认仍为 1000，拒绝 0/负数/uint32 溢出，共享环境构造保证两模式一致传入并在摘要记录。100000 是实验排障值，不是生产建议。

### 控制组：60s+30s，六次全部通过

|总连接|持久化成功 RPS|无 DB 成功 RPS|结论|
|---:|---:|---:|---|
|128|1275.74|1275.74|通过|
|192|1913.60|1913.60|通过|
|256|2551.46|2551.46|通过|

核对全部生成配置及日志：网关 100000、Scene 1000，无 frame rate exceeded；三档持久化位置/版本、背包及显式重登核验通过。256 持久化最大客户端 p99 45.090ms。与先前网关 1000 配置对比，192/256 的失败消失，支持网关帧保护是这两档原断链的直接触发因素；单次对照不是长期稳定性保证。

### 更高档：120s+30s 排空+30s 额外观察

|总连接|持久化成功 RPS|无 DB 成功 RPS|持久化/无 DB 最大 p99(ms)|持久化/无 DB 错误数|
|---:|---:|---:|---:|---:|
|384|2194.92|3833.60|475.590 / 125.755|1493 / 0|
|512|2138.41|849.15|712.943 / 3611.045|2222 / 8010|
|768|872.39|86.73|3790.613 / 10976.406|4778 / 15461|
|1024|177.57|0|7474.157 / 不适用|7073 / 1024（准入失败）|

八次评价均失败，但全部继续执行。384 无 DB 吞吐达标、零错误，仅 p99 超出 100ms 评价线；其余有效高档出现业务错误、吞吐下降。日志未见网关或 Scene 的帧率超限；出现 sendToWorker queue full 及 Gateway 路由超时，说明移除原门槛后暴露了不同瓶颈。不能把日志数量视为独立丢失请求数，频繁告警本身也会增加负载；业务错误含 unclassified，其精确来源尚未逐一归因。1024 无 DB 未通过 READY，不能作为成功负载饱和点。

修正离线恢复判据：代码 `/home/lanxiyuan/Project_Cpp/GameServer/Zone/gatewayServer/serviceRegistry.cc` 的 ReadyCount 统计两个 Scene 注册身份，不是四条 channel，原报告硬编码 >=4 会把健康链路误报为未恢复。本轮补 RED→GREEN 回归后采用 >=2，同时要求完整 worker/netQueue、在途和 pendingRoutes 为零。该判据仍不能保证全部通道健康或证明新请求成功。

各档两客户端的停发后首次满足采样条件时间：384 持久化 11/12s、无 DB 1/1s；512 持久化 9/5s、无 DB 8/11s；768 持久化 13/16s、无 DB 15/15s；1024 持久化 17/15s，无 DB 无有效恢复观测。已启动的七次场景额外观察各 31 条快照，最后两个 Scene 的 workerQueueDepth/netQueue/playerCount 均为零，两个 Gateway backendReady 均为 2。高档持久化最终完整性验证产物缺失，不能宣称 SQL/背包/重登通过。

结论：**提高网关阈值在本轮确实解决了 192/256 两模式原失败，但未让所有更高档通过。** 当前可用边界只可描述为 256 总连接单次控制成功、384 总连接长测退化；测试时长不同，未完成边界重复细化。无 DB 的最佳观测成功吞吐约 3834 RPS；不能把它称为已精确定位的物理极限。闭环、同机发压、密集 AOI 和日志开销均限制外推。

证据目录：
- 控制：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-1792211828045914812/`
- 扩容及恢复：`/home/lanxiyuan/Project_Cpp/GameServer/Zone/build-acceptance/persistence-staircase-1792212492011814284/`
- 各含 summary/result/boundary-report 及原始指标、资源和恢复时序。旧日志和旧报告不篡改；旧批 backendReady=0 的失败结论不受此次身份计数修正影响。
