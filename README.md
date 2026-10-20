# ClearMoon GameServer

基于 C++20、Lua 和 Protobuf 的 MMORPG 分区分服服务端实验项目。本文只介绍顶层架构、网关与场景服设计、区服关系，以及已有测试的能力边界。实现、配置、构建与运行细节见各服务文档。

## 顶层架构

```text
客户端 ┄┄> LoginServer（待实现：账号认证、登录凭证、区服列表与角色归属查询）
  │
  │ ClientFrame：客户端主动连接所选区服的网关
  ▼
┌────────────────────── 一个 Zone（zoneId）──────────────────────┐
│ Gateway 1 … Gateway G                                        │
│     ▲ GateFrame：后端主动连接，网关经该连接转发业务             │
│     ├── Scene 1 … Scene S：玩法、玩家状态、地图与 AOI           │
│     ├── Chat：同一区服跨网关私聊与喊话                         │
│     └── Global：当前仅 Ping/Echo 接入验证                      │
│                                                              │
│ Scene 内部：网络线程 ↔ MsgBus ↔ 逻辑 worker / Lua / AOI         │
│                            ↔ DB、定时器等辅助线程             │
│ Scene 数据通道：MySQL（角色/快照）、Redis（按区服隔离的键）     │
└──────────────────────────────────────────────────────────────┘
```

LoginServer **待实现**，图中虚线仅表示规划职责，不代表已接通正式登录或入区鉴权。当前网关使用开发期鉴权机制，不能替代正式身份系统。

## Gateway：接入、会话与路由

- 客户端和后端分别使用独立监听入口及 `ClientFrame`、`GateFrame` 协议，不能混用。
- 维护客户端会话、可信玩家身份、服务注册、场景/通道绑定和有界回程路由，不执行玩法或直接访问玩家数据库。
- 后端注册包含服务及区服身份；多 Scene 按 `sceneId` 区分。会话选定具体场景通道后保持绑定，不提供自动迁移或故障接管。
- 转发时分配内部 `seq`，保存客户端 `requestId` 与会话；响应校验来源和身份后恢复客户端请求号。后端推送由网关筛选本地会话并投递。
- 场景业务携带版本化玩家绑定信封。首次声明先提交 ONLINE，稳定绑定后只提交带信封的业务帧；退出发送精确 OFFLINE。声明仅表示交给本地发送路径，不代表场景服已应用。

## Scene：状态所有权与异步数据通道

- 不直接接收客户端连接；网络线程维护网关连接与玩家绑定索引，通过进程内 `MsgBus` 投递到所属逻辑 worker。
- 每个 worker 独占玩家对象、Lua VM、持久化协调器和 AOI 状态；C++ 管理生命周期和基础校验，Lua 承载移动等示例玩法。worker 之间传事件与快照，不共享可变 Player。
- 同一 Scene 进程内通过消息总线同步跨 worker AOI 副本与恢复快照。观察者处理生成视野推送，经网络队列和网关返回；本地队列接受不等于客户端收到。
- DB 线程异步执行 MySQL/Redis 任务，结果回投原 worker。保存使用固定快照、版本校验、去重和 ACK；账本在内存中，不提供崩溃后任务恢复或未知 COMMIT 结果自动对账。
- `PendingDbTasks` 活跃索引只调度仍需处理的任务，保留历史身份及重复结果语义；它是维护成本优化，不等于提高了已测最大吞吐容量。

### 一次场景请求

```mermaid
sequenceDiagram
    participant C as 客户端
    participant G as Gateway
    participant S as Scene 网络线程
    participant W as 玩家所属 worker
    S->>G: 后端连接与注册（zoneId / sceneId）
    C->>G: ClientFrame Request(requestId)
    G->>G: 校验会话，分配 internalSeq，保存回程路由
    G->>S: 首次绑定 ONLINE；带信封的 GateFrame 业务
    S->>W: MsgBus 投递，校验绑定及业务路由
    W->>W: C++ / Lua 玩法与 AOI 状态处理
    W-->>S: 响应进入网络发送队列
    S-->>G: GateFrame Response(internalSeq)
    G-->>C: 恢复 requestId 的 ClientFrame Response
```

## 多网关、多场景与多区服的关系

| 层级 | 当前关系与边界 |
| --- | --- |
| 一个 Zone 内的多个 Gateway | 汇聚各自客户端；后端按配置连接网关。网关不自动共享完整在线会话表。 |
| 一个 Zone 内的多个 Scene | 网关按场景与通道绑定路由；跨 worker AOI 只在同一 Scene 进程内同步，不是跨 Scene 迁移。 |
| Chat 与多个 Gateway | Chat 连接同一区服已配置网关，私聊/喊话向网关扇出，各网关筛选本地会话；没有离线消息或可靠送达确认。 |
| Global | 当前只连接一个网关，提供 Ping/Echo；“全局”不代表已实现跨区中心、公会或排行榜。 |
| 不同 Zone | 以 `zoneId` 区分注册与数据命名空间；Redis 键使用 `zone:{zoneId}:...`。MySQL 的库、账号及部署隔离需单独配置，不能仅靠 zoneId 自动保证。 |
| 跨区服业务 | 当前没有跨区聊天、跨区 AOI、角色迁移或统一跨区业务总线。LoginServer 待实现，职责包括提供区服列表与角色归属查询。 |

同一区服增加 Gateway/Scene 是横向部署，不等于不同区服已经互通。MySQL/Redis 是数据通道，不是已实现的跨区实时消息总线。

## 3G2S 最新测试摘要（2026-10-20）

3G2S 指 **3 个 Gateway + 2 个 Scene**。最新有效测量采用同机 Release、每 Scene 4 个 worker、AOI 与持久化开启（每秒保存），执行单连接单在途的闭环 Walk 负载。下表成功 RPS 为客户端正式负载窗口内成功响应数除以窗口时长，不含 drain；不是内部 handled 消息数，也不是单 Scene 吞吐。p99 合并三个客户端的成功响应延迟样本计算，含排空收到的成功响应。

| 归档轮次 | 连接与供给目标 | 正式窗口 | 成功 RPS | p99 | 业务错误 / unresolved | 严格结果 |
| --- | --- | --- | ---: | ---: | --- | --- |
| 2026-10-20：事件驱动改造前的旧客户端 | 512 × 30 RPS = 15,360 RPS | 60 秒 | 15,358.83 | 96.772 ms | 0 / 0 | 通过 |
| 2026-10-20：事件驱动改造前的旧客户端 | 512 × 30 RPS = 15,360 RPS | 600 秒 | 15,353.17 | 94.335 ms | 605 次 `1011:server busy` / 0 | 未通过 |

两档均完成 512 人登出，请求记账及成功延迟样本数量匹配，两个 Scene 正常退出，并取得全部受测玩家在线保存版本推进的证据；但 capacity 模式未执行最终坐标逐玩家核对、背包一致性或重登恢复验证。600 秒档最大发送调度滞后约 **28.23 秒**，存在分钟级吞吐波动；Scene 1/2 的 RSS 分别由 **29.29 → 157.84 MiB / 29.34 → 146.34 MiB**，未观察到平台期，不能称为内存长稳通过，也不能仅凭 RSS 判定泄漏。两档不是优化前后 A/B，不能用其差异推断优化收益。

**后续 epoll 客户端的万连接尝试未形成有效容量测量：** 10,240 连接 × 1 RPS（计划 60 秒）及 × 1/3/5 RPS（各计划 600 秒）均完成认证和进场准备，但三个客户端均在预热阶段非零退出，未进入正式窗口，正式成功 RPS 与 p99 均为 **N/A**。正式请求/错误为 0 不代表零错误通过；Scene 退出时有持久化排空不完整记录。现有证据不足以确定首个退出触发原因，不能据此判定万连接容量通过或不支持，也不能把旧客户端的有效吞吐结论直接转移给当前客户端基线。

**可以确认：两个 Scene 合计在特定移动/AOI 场景下已展示约 1.535 万端到端成功 RPS，60 秒严格零错误通过；600 秒虽完成测量，但有 busy、调度滞后及内存增长，不构成完整长稳验收。** 万级 RPS 不等于万连接能力；最大容量未测定，不外推单 Scene、混合业务或小时级稳定性。

活跃索引的独立 256 连接历史测试供给封顶约 5,120 RPS，成功吞吐仅 5,119.17 → 5,119.88 RPS；主要证据仍是维护热点与 CPU 成本下降，不能将上述吞吐归为索引收益。最新有效测量证据位于本地 `/home/lanxiyuan/Project_Cpp/GameServer/docs/benchmark-results/approved-three-cases-20261020-102455/`；后续无效容量测量见同级 `reactor-10240x1-60s-20261020-112349/` 与 `reactor-10240-r1-r3-r5-600s-20261020-113305/`。这些目录受 `.gitignore` 忽略，不保证随仓库检出提供；历史诊断时间线已部分无损压缩、运行日志已精简，历史 perf 收尾仍有 exit=-15 限制。

## 文档导航

| 文档 | 维护范围 |
| --- | --- |
| [Gateway](Zone/gatewayServer/README.md) | 接入、会话、路由、配置与运行 |
| [Scene](Zone/sceneServer/README.md) | worker、Lua、AOI、持久化与运行 |
| [Chat](Zone/chatServer/README.md) / [Global](Zone/globalServer/README.md) | 当前业务与职责限制 |
| [线协议](Zone/PROTOCOL.md) | ClientFrame / GateFrame 与业务消息 |
| [监测模块](Zone/sceneServer/src-cpp/metrics/README.md) | 指标、诊断和调度策略 |
| [持久化验收](Zone/sceneServer/tests/PERSISTENCE_ACCEPTANCE.md) | 隔离数据库验证方法与历史结果 |

## 仓库内容与本地文件边界

以 [.gitignore](.gitignore) 和 Git 跟踪状态为准：默认入口是 `Zone/` 源码、各服务 README、线协议和已跟踪验收文档，不依赖本机压测目录。

| 忽略规则 | 内容与说明 |
| --- | --- |
| `docs/*` | 本地专题、历史报告、性能证据和计划；不保证随 clone 提供，不能将本地链接当作仓库已交付文档。 |
| `LoginServer/` | 登录服目录被忽略；LoginServer 待实现，仅承担账号认证、登录凭证、区服列表与角色归属查询职责。 |
| `Zone/build`、`Zone/build-acceptance/`、`Zone/build-readme/`、`Zone/build-worker-pending/`、`Zone/build-worker-pending-release/` | 本地构建与测试输出，需自行生成；当前规则逐项列举，并非通配忽略所有 `Zone/build*`。 |
| `.cline/`、`.cache/`、`.VSCodeCounter/*` | 本地工具、缓存和编辑器统计。 |
| `Zone/*.log`、`Zone/compile_commands.json` | 指定位置的日志/编译数据库；不代表所有目录的日志均被忽略。 |
| `docker-data/`、`Enviromation.md` | 本地容器数据、机器环境记录，不是项目通用运行要求。 |

`.gitignore` 不会自动取消已有文件的跟踪，也不等于文件已删除。根目录 `logs/` 不在当前忽略规则内，已清空的日志未来仍可能重新生成；不要将日志、凭据、证书私钥或测试数据库当作源码交付。本文不修改忽略规则、不强制纳入本地文档。测试数字是当前工作区归档观测，不保证仅 checkout HEAD 即可复现。
