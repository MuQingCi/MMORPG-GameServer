# 场景服务器（sceneServer）

> 同步日期：2026-10-19。本文按当前工作区维护，不代表已提交版本；`docs/*`、`LoginServer/` 和指定构建目录受根 `.gitignore` 忽略。本地证据链接不保证随仓库检出提供，历史测试不等于本轮重跑。

## 约定
- 场景服是内部服务，不直接接收客户端 `ClientFrame`。默认主动连接网关，`ConnNew/ConnClose` 表示内部 TCP 链路事件，不能等同于某个玩家上下线。
- 玩家上线/下线由网关版本化绑定信封表达；网络层校验后通过消息总线投递到玩家所属逻辑 worker。普通玩法请求只能使用已有绑定，不能隐式创建 Player。
- 当前入站玩法为进场、移动、示例路径和退出；已接入 AOI 观察者差集/全量视野推送，战斗尚未实现。
- 客户端 `ViewResyncReq` 已登记入站路由，由所属 worker 标记视野重同步；它与跨 worker owner 快照恢复不是同一协议。
- 下文 Mermaid 图展示当前数据流。AOI 推送进入本地发送队列不等于客户端收到，也不表示具备可靠送达或跨进程场景迁移能力。

## 内部功能

### 网关玩家绑定（新增，需网关与场景服同步升级）

- GateFrame v1 的 32 字节头不变；网关发往 `kScene` 的玩家消息 body 使用 `base/gatewayPlayer.h` 中的版本化信封。场景服剥离信封后才将玩法载荷交给 Lua，不接受旧的裸 body 玩家请求。
- 信封为小端：magic(u16=0x4750)、version(u16=1)、playerId(u64)、clientSession(u64)、clientEpoch(u32)、gatewayId(u32)、sceneId(u32)，共 32 字节，随后为原始玩法载荷。
- 网关专用 module=200，method=1/2 表示 ONLINE/OFFLINE，载荷为空、seq=0。该路由不允许公网客户端指定。ONLINE 表示有效会话绑定，不表示数据加载完成或 AOI 场景准入成功。
- `NetServer` 独占 `GatewayClientIndex`：网关连接 session -> 多个玩家绑定，另有玩家到连接的反向索引及历史世代。普通请求只校验绑定，不能创建/换绑；离线精确匹配；断链为该连接的全部有效绑定产生逐玩家离线通知。
- 生命周期向 worker 投递失败会排队按顺序重试；普通业务使用有条数/字节/期限预算的积压，拒绝或过期时按协议返回 busy/timeout，不自动重放非幂等业务。生命周期积压及历史身份仍需容量/租约治理，不能宣称全系统内存有界。
- `onNetMsg()` 先处理网关绑定通知。只有 ONLINE 创建/换绑 Player，普通合法玩法路由也不能隐式创建 Actor。Player 保存当前 ClientData；`Player::session()` 仍是场景服网络连接，不是客户端 session。`clientEpoch` 与 Actor epoch 分开使用。
- `ConnClose` 仅处理内部链路日志，不再删除某个“最后出现的玩家”。当前断链策略为该连接绑定的玩家全部下线；业务需要断线保留/租约等待时应调整此策略。
- 登录业务须在数据加载与准入成功后调用 `player.enter_scene`；绑定鉴权不自动进入 AOI。进场后使用已验证的 clientEpoch 注册观察者。网络层不读取 Player 或 AOI。

网关按实际场景通道进行 ONLINE 声明，首次声明可与首条业务合帧提交；稳定绑定后的业务只携带绑定信封，不再每次重复 ONLINE。重复 ONLINE 不清空视野基线、不提升 Actor epoch；声明成功只表示本地提交，不证明断线重连后的可靠在线集合恢复。

当前限制：gatewayId 必须稳定且区服内唯一；跨网关覆盖拒绝，尚未实现权威接管凭证。网关本地 epoch 重启回绕、后端连接重连后同 epoch 恢复、可靠在线集合对齐尚未实现。网关 TCP send 不承诺离线通知已应用；链路关闭由场景服逐绑定清理兜底，但进程崩溃、连接存活而通知丢失仍需租约/快照。网关支持按 sceneId 区分多个场景实例，但会话选定后固定到具体通道，不支持自动迁移或故障接管。

- **面向网关**：主动连接区服共享配置中的网关内网地址，使用 `GateFrame` 握手、收发业务消息；本地直连监听默认关闭。
- **面向玩法**：按 `(module, method)` 路由 protobuf 请求，在玩家所属的逻辑线程中执行 Lua 处理器并生成响应或推送。
- **面向数据和定时任务**：通过消息总线异步处理 MySQL/Redis 任务、定时回调、玩家状态保存与 Lua 热更新。

## 职责边界

- 客户端只连接网关的 `ClientFrame` 端口；场景服处理服务间 `GateFrame`，不负责客户端鉴权、登录票据验证或公网接入。
- `NetServer` 持有网络连接与编解码，`LogicThread` 持有各自的 Lua VM 和玩家分片；数据库与定时器线程各司其职，跨线程只通过 `MsgBus` 投递消息。
- 按 `playerId` 路由玩家请求到所属 worker；没有玩家 ID 时按会话分配。无效的 `(module, method)` 在创建玩家 Actor 之前拒绝。
- 仅实现路由表中登记的玩法请求。进场/退出由 C++ 处理，移动/示例路径由 Lua 处理；其他未实现处理器明确返回错误。`net.listenEnable` 仅供本地联调/工具，不应作为公网客户端入口。

## 与 Client 的消息流

场景服**不直接收发**客户端帧，客户端先经网关鉴权和转发：

客户端链路当前使用 `ClientFrame v2`（32 字节头、版本 2、12 字节角色名）；
场景服仍仅接收 `GateFrame v1`，不会解析客户端角色名。

```text
Client -- ClientFrame(Request) --> Gateway -- GateFrame --> SceneServer
Client <-- ClientFrame(Response/Push) <-- Gateway <-- GateFrame <-- SceneServer
```

### 请求与响应数据流图

```mermaid
flowchart TD
    C[客户端] -->|ClientFrame v2 Request / requestId| G[网关：鉴权会话与回程路由]
    G -->|GateFrame v1：ONLINE + 带信封的请求| N[NetServer::onFrame]
    N --> I[校验来源、sceneId、GatewayClientIndex；剥离信封]
    I -->|MSGTYPE_CONN_DATA / 按 playerId 分片| Q[MsgBus worker 队列]
    Q --> L[LogicThread::onNetMsg]
    L -->|module=200| B[onPlayerBinding：创建、换绑或清理 Player]
    L -->|普通业务| R[RouteTable + 当前 Player 绑定校验]
    R -->|3/4 进场；3/5 退出| P[C++ PlayerService / PlayerManager]
    R -->|3/2 移动；3/3 示例路径| U[LuaEnv::DispatchC2S]
    U -->|player.move| P
    R -->|无效路由或绑定| E[RetTip 错误响应]
    P --> A[Player 与本地 AOI 权威状态]
    P --> O[响应序列化]
    U -->|net.send / 保留请求 seq| O
    E --> O
    O -->|MSGTYPE_SEND| T[MsgBus 网络发送队列]
    T --> S[NetServer::sendTo]
    S -->|GateFrame：保留 seq/playerId/module/method| G
    G -->|恢复 requestId / ClientFrame Response| C
```

### 场景服启动与网关注册时序

场景服由 `SceneServer::start()` 负责按依赖顺序创建消息总线、网络线程、定时器、可选 DB 通道和逻辑 worker，最后由 `NetServer::connectTo()` 主动连接网关。网络连接建立后，场景服发送 `GateFrame` 握手；网关注册成功后才进入正常业务收发。

```mermaid
sequenceDiagram
    participant M as SceneServer::start
    participant B as MsgBus
    participant N as NetServer
    participant T as TimerThread
    participant DB as DBThread
    participant L as LogicThread
    participant G as gatewayServer

    M->>B: 创建 MsgBus(worker 数、Redis 分片数)
    M->>N: start()
    N->>N: 创建 epoll/eventfd；按配置决定本地监听
    M->>T: start()
    M->>DB: start()（dbEnable=true 时）
    M->>L: 创建并 start() 每个逻辑 worker/Lua VM
    M->>N: connectTo(gateway)
    N->>G: TCP connect(privateConfig)
    N->>G: GateFrame SYS/Handshake(serviceId=8, zoneId, sceneId)
    G->>G: 校验白名单和 zoneId，注册后端连接
    G-->>N: 后续允许业务帧（当前握手无协议 Ack）
```

### 玩家绑定与一条玩法请求的运行时序

ONLINE/OFFLINE 和普通玩法请求都先经过网络线程的信封校验。网络线程只负责协议、连接索引和投递，不访问 `Player` 或 AOI；逻辑 worker 是玩家状态的唯一执行上下文。

```mermaid
sequenceDiagram
    participant G as gatewayServer
    participant N as NetServer
    participant Q as MsgBus
    participant L as LogicThread
    participant P as Player/PlayerService
    participant U as LuaEnv
    participant O as AOI/WorkerPushSink

    G->>N: GateFrame ONLINE
    N->>N: Decode；Unpack GatewayPlayer envelope；GatewayClientIndex::Bind
    N->>Q: sendToWorkerByPlayerId(MSGTYPE_CONN_DATA)
    Q->>L: onNetMsg -> onPlayerBinding
    L->>P: 创建/换绑 Player；记录 ClientData

    G->>N: GateFrame 玩法请求(seq, playerId, binding, payload)
    N->>N: 校验来源、sceneId、当前绑定和玩家世代
    N->>Q: sendToWorkerByPlayerId(MSGTYPE_CONN_DATA)
    Q->>L: onNetMsg
    L->>L: RouteTable 校验（失败则 RetTip，不创建 Actor）
    alt C++ 入口：进场/退出
        L->>P: PlayerService::EnterScene / LeaveScene
        P->>O: 更新 AOI 权威实体
    else Lua 入口：移动/示例路径
        L->>U: LuaEnv::DispatchC2S
        U->>P: player.move 等绑定调用
        P->>O: 地图校验并更新 AOI
    end
    L->>Q: sendToNet(MSGTYPE_SEND，保留 seq/playerId/module/method)
    Q->>N: processBusMsg -> sendTo
    N-->>G: GateFrame Response/Push
```

### 断链和 OFFLINE 清理时序

内部 TCP 连接断开本身不等于某个玩家下线。只有带完整绑定信封的 OFFLINE，或网络线程枚举该网关连接上的全部绑定后逐玩家生成的离线通知，才会进入玩家生命周期处理。

```mermaid
sequenceDiagram
    participant G as gatewayServer
    participant N as NetServer
    participant I as GatewayClientIndex
    participant Q as MsgBus
    participant L as LogicThread
    participant P as PlayerPersistence/AOI

    G-xN: GateFrame 链路断开
    N->>I: Close(connection)
    I-->>N: 返回该连接上的全部有效 ClientData
    loop 每个绑定玩家
        N->>Q: 投递 OFFLINE；队列满则进入 pendingClientNotifications_
        Q->>L: onPlayerBinding(OFFLINE)
        L->>P: LeaveScene；执行最终快照/清理流程
    end
    N->>N: 清理连接、sessionWorker 和网络资源
```

`Player::session()` 是场景服与网关的内部连接 session；信封中的 `clientSession` 是网关客户端会话。两者不能互换。网络发送入队成功不代表客户端已经收到，ONLINE 也没有场景准入成功的含义。

网关从已鉴权会话写入 `playerId`、内部 `seq` 和目标服务号；场景服接收业务帧并回复到原连接。网关要求普通响应保持原请求的 `seq/playerId/module/method`，才能按回程路由还原客户端 `requestId`。详见 `Zone/gatewayServer/README.md` 与 `Zone/PROTOCOL.md`。

### 消息定义

服务间帧固定头 **32 B**，魔数 `0xC1EA`、版本 `1`：

| 字段 | 长度 | 含义 |
| --- | ---: | --- |
| magic / version | 2 / 2 B | 协议标识与版本 |
| msgType / retrFlag | 1 / 1 B | 链路类型和重试标记；`msgType` 不是请求/响应类别 |
| module / method | 2 / 2 B | 系统或玩法模块与方法 |
| seq / playerId | 8 / 8 B | 请求关联号、玩家 ID；主动推送 `seq=0` |
| totalLen / srcServiceID / dstServiceID | 4 / 1 / 1 B | 总长度含帧头，源和目的服务号 |
| body | 可变 | 握手或玩法数据；整帧上限 10 MiB |

头部多字节整数为**网络序（大端）**；握手体里的 `zoneId`、`sceneId` 为**小端**。定义见 `Zone/zone_common/base/proto.h`，当前路由表位于 `Zone/sceneServer/src-cpp/routeTable.cc`，不是由示例路由 YAML 动态加载。

| module/method | 请求 → 响应 | 处理位置 |
| --- | --- | --- |
| `3/2` | `gs.WalkReq` → `gs.WalkAck` | Lua `move.c2s_walk`，经 PlayerService 更新权威位置/AOI |
| `3/3` | `gs.PathReq` → `gs.PathAck` | Lua `move.c2s_path`，返回固定示例路径，不是真实寻路 |
| `3/4` | `gs.EnterSceneReq` → `gs.EnterSceneAck` | C++ 进场处理 |
| `3/5` | `gs.LogoutReq` → `gs.LogoutAck` | C++ 退出处理 |
| `200/1`、`200/2` | ONLINE、OFFLINE，无业务载荷，`seq=0` | 网关专用生命周期处理，不开放给客户端 |

另有仅出站的 `gs.SCPushState`、`gs.RetTip` 路由；AOI 视野消息以当前 Protobuf 和协议文档为准，路由登记或发送入队不等于客户端已收到。

### 场景服将结果返回给客户端

```text
GateFrame --> NetServer::onFrame --> MsgBus（玩家/会话所属 worker）
          --> LogicThread::onNetMsg --> RouteTable::FindC2S
          --> LuaEnv::DispatchC2S --> MsgBus 网络发送队列
          --> NetServer::sendTo --> GateFrame --> Gateway --> ClientFrame
```

无效路由会返回 `gs.RetTip{code=1001,text="route not found"}` 且不创建 Actor。关联请求的错误响应沿用原请求的 `module/method/seq/playerId`，因此能通过网关回程路由并恢复客户端 `requestId`；客户端需在该请求的 Response body 中识别 `gs.RetTip`（当前服务间协议没有显式错误类别）。无关联请求的 RetTip 通知仍使用 SYS 模块/方法。

## 可靠 DB 任务投递与玩家持久化

### 运行对象与所有权

- `LogicThread` 是玩家状态和 `PlayerPersistence` 的唯一执行者。每个逻辑 worker 独占自己的 `PlayerManager`、Lua VM、持久化记录和 `PendingDbTasks`，其它线程不能直接修改 `Player`。
- `PendingDbTasks` 位于逻辑 worker，负责在发送 DB 任务前登记固定请求、分配 `taskId`、处理消息队列背压，并保存尚未完成的任务。任务一旦被 DB 队列接受，后续超时不会重新执行原 SQL/Redis 命令。
- `DBThread` 按 MySQL 通道和 Redis 分片消费任务。每条 DB 通道独占一个 `DbResultLedger`，记录请求身份、执行结果和 ACK 状态；账本只在所属 DB 线程访问。
- `PlayerPersistence` 保存玩家快照的 active/queued 版本、捕获 revision、生命周期 token 和错误状态。它只负责快照串行化和保存责任，不直接执行 SQL；实际执行由 `DBThread` 完成。
- `MsgBus` 只传递 `Msg`，不传递 `Player` 指针。DB 结果必须回到发起任务的 `srcWorkerId`，由所属 `LogicThread` 校验 epoch、taskId 和快照身份后应用。

### 一次保存任务的生命周期

```mermaid
sequenceDiagram
    participant L as LogicThread
    participant P as PlayerPersistence
    participant Q as PendingDbTasks
    participant B as MsgBus
    participant D as DBThread
    participant R as DbResultLedger

    L->>P: request(player, offline, submit)
    P->>P: freezeSaveSnapshot(version, token)
    P->>Q: submit(DB task with taskId)
    Q->>Q: 先登记 PendingDispatch
    Q->>B: sendToDb(task)
    alt 队列背压
        B-->>Q: kBackpressure
        Q->>Q: 保留任务，稍后重试入队
    else 入队成功
        B-->>Q: kAccepted
        Q->>Q: AwaitResult；不因超时重发 SQL
        B->>D: DB task
        D->>R: accept(task)；按 taskId 去重
        D->>D: 执行 SQL/事务/Redis 命令
        D->>R: finish(task, result)
        R->>B: 回投 DB result
        alt 结果回投背压
            B-->>R: 未接受
            R->>R: 保留结果并重试投递
        else worker 接受结果
            B->>L: DB result
            L->>Q: complete(result)
            L->>P: complete(taskId, result, livePlayer)
            L->>B: 发送 ACK control
            B->>D: DB ACK
            D->>R: 标记 acknowledged，允许容量回收
        end
    end
```

关键不变量：

1. `PendingDbTasks` 在第一次发送前登记请求；同一 `taskId` 的不同请求会被拒绝，避免身份冲突。
2. “已入 DB 队列”与“DB 已执行”是两个状态；队列接受不等于事务成功，结果必须带回原始 `taskId`、`playerId`、`epoch`、`srcWorkerId`、模块和方法字段。
3. DB 结果回投失败只重试结果，不重新执行原任务；重复结果只触发 ACK，不重复应用玩家或 Lua 业务逻辑。
4. ACK 表示结果已经由 worker 接管并完成传递层确认，不替代业务层的保存结果校验。玩家保存还会校验 `saveVersion`、`capturedRevision` 和 `saveLifecycleToken`。
5. 未知状态不能证明原 SQL 尚未提交，因此状态查询不会自动重放原任务；不确定结果由持久化协调器保留并阻塞后续不安全的保存。

### 周期保存与停服排空

周期保存由 `LogicThread::run()` 每轮主循环检查 `flushIntervalMs`，不依赖 `onIdle()`。因此逻辑 worker 即使持续处理消息、队列始终非空，也会扫描 dirty 玩家并创建快照任务。加载中的玩家、排空中的玩家和未启用持久化的实例不会生成普通周期保存。

停服时的顺序是：停止接收新的业务生命周期 → 各逻辑 worker 进入排空 → 登出玩家并提交最终快照 → 等待 `PlayerPersistence`、`PendingDbTasks` 和结果 ACK 全部稳定，或达到 `shutdownDrainMs` → 最后停止 DB 线程。排空未完成时记录错误并将 worker 标记为未成功排空，不能把进程退出当作数据已经持久化。

### 能力边界

当前可靠性范围是**单进程内的有界内存任务登记、队列背压重试、DB 结果回投重试、幂等去重和 ACK 回收**。`PendingDbTasks` 与 `DbResultLedger` 不落盘，因此不提供进程崩溃后的任务恢复、跨重启去重、跨网关接管或 COMMIT 断链未知结果的自动对账。真实数据库验收也不等同于任意 MySQL/MariaDB 版本兼容性证明。

### 活跃任务调度索引

Worker 构造时从 `SceneServer.db.activeIndex` 选择 `PendingDbTasks::SchedulingPolicy`：`true` 使用活跃索引，`false` 使用完整扫描。仓库 `Zone/sceneServer/config/sceneConfig.yaml` 当前显式设置 `true`，结构缺省仍为 `false`。这是场景本地 DB 调度键，不受共享 DB 配置迁移禁令影响；其他迁移 DB 键仍拒绝。旧环境变量 `SCENE_EXPERIMENT_ACTIVE_DB_INDEX` 不再控制策略，不支持热切换。

活跃索引集中维护需要投递、等待结果或待 ACK 的任务；失败项及时退出调度，但保留历史身份与原 `unsettled()` 语义。完整历史、60 秒回收窗口、ACK 背压及重复结果重新激活 ACK 语义不变。已有任务后 `enableActiveIndex()` 返回 `false`，不会静默切换；接口及 Sender 仅在所属线程调用，Sender 不可重入修改同一实例。启动日志记录 `active-index` / `legacy-scan`。

2026-10-19 的 3G2S、256 × 20 RPS、120 秒归档：成功吞吐 5,119.17 → 5,119.88 RPS（约 +0.014%），两 Scene 平均 CPU 约 4.02 → 2.48 核（原始值观测下降约 38.35%），p99 39.384 → 26.881 ms。后段两个维护函数 Self 合计 63.70% → 1.34%；固定 32 活跃/16,384 历史微基准 CPU/轮 325.017 → 0.259 μs（约 -99.92%），不是业务吞吐增幅。测试供给封顶且玩家分布不同，最大容量收益未测定。

见[热点报告](../../docs/benchmark-results/busy-hotspot-profile-20261019/report.md)、[加固台账](../../docs/benchmark-results/active-index-hardening-20261019/execution-ledger.md)、[监测模块](src-cpp/metrics/README.md)及[压测索引](../../docs/benchmark-results/README.md)。历史报告中的实验开关状态只适用于当时二进制。

## 与 BackendServer 的消息流

场景服本身是网关的后端服务：启动后从 `ZoneServer.gateways` 读取地址，**反向连接网关内网端口**，发送 `GateFrame(module=SYS, method=kHandshake)`。握手体是公共 `BackendHandshake` 定义的版本化 27 B 结构，包含服务类型、区服、场景实例、进程世代、目标网关和通道编号。出站 `srcServiceID` 取 `SceneServer.serviceId`，示例为 `8` (`ServerID::kScene`)，表示服务类型而非实例编号。网关检查区服 ID 与白名单，注册前不接受业务帧；不同 sceneId 可并存，同实例同通道重连替换旧连接。

```text
SceneServer NetServer -- 握手 GateFrame --> Gateway privateConfig 端口
Gateway -- 业务 GateFrame(dst=SceneServer.serviceId) --> NetServer
NetServer -- 响应/推送 GateFrame(dst=ServerID::kGateway) --> Gateway
```

网络层处理非阻塞收发、帧解码及缓冲/帧速率限制；支持同服务类型的多场景实例接入，但当前没有完整的后端心跳、自动迁移或故障接管能力。

### 玩家绑定与断链清理数据流图

```mermaid
flowchart TD
    G[网关] -->|ONLINE：200/1；seq=0；绑定信封| N[NetServer / GatewayClientIndex]
    G -->|OFFLINE：200/2；精确绑定| N
    X[网关 TCP 连接关闭] --> D[枚举此连接的全部有效玩家绑定]
    D -->|逐玩家合成 OFFLINE| Q[生命周期待补队列 / MsgBus]
    N -->|来源、世代与绑定校验成功| Q
    Q -->|按 playerId 投递；队列满则重试| L[LogicThread::onPlayerBinding]
    L -->|新的有效 ONLINE| P[创建或换绑 Player]
    L -->|重复 ONLINE| K[保留当前 Player / epoch / 基线]
    L -->|匹配 OFFLINE| O[注销观察者 / logoutPlayer]
    O --> A[离开 AOI；清理定时器与 Player]
    P --> W[等待显式进场请求]
    W -->|EnterSceneReq| E[建立 AOI 实体并注册观察者]
```

网络层先更新绑定索引，再排队向逻辑层投递生命周期。待补队列积压时普通玩法可能被丢弃，不自动重放。主动 `LogoutReq` 走玩法分支清理 Player；它不是客户端直接发送 OFFLINE，也不等同于注销网关鉴权会话。

## 内部组件

| 组件 | 职责 |
| --- | --- |
| `main.cc` / `ZoneConfig` / `SceneConfig` | 加载共享与场景专属配置、合并校验、初始化日志和路由表、等待信号。 |
| `SceneServer` | 组装和管理总线、网络、定时器、DB 和逻辑 worker；热更新 Lua。 |
| `NetServer` | 网关反向连接、可选本地监听、握手及 `GateFrame` 收发。 |
| `MsgBus` | 多条 worker 队列、网络发送队列、MySQL 队列与 Redis 分片队列。 |
| `LogicThread` / `PlayerManager` | 玩家 Actor 生命周期、路由校验、Lua 玩法处理；每个 worker 独占自己的 Lua VM。 |
| `RouteTable` / `LuaEnv` | `(module,method)` 与 protobuf 类型、Lua 函数的映射和调度。 |
| `DBThread` / `TimerThread` | 异步 DB 任务及定时器回投到所属逻辑线程。 |
| `PlayerService` / `AoiService` | 权威玩家位置、实体生命周期、观察者状态与 AOI tick。 |
| `WorkerPushSink` / `AoiServiceRouter` / `AoiWire` | 内部 AOI 事件编码、跨 worker 投递、可信发送方校验与快照恢复。 |

主要接口：`SceneServer::start/stop` 控制生命周期，`reloadLua` 广播热更新，`stats` 输出各 worker、定时器与 DB 快照；`MsgBus::sendToWorkerByPlayerId/sendToNet/sendToDb` 执行跨线程投递。生命周期细节见 `Zone/sceneServer/src-cpp/sceneServer.h`。

### DB、定时器与热更新数据流图

```mermaid
flowchart LR
    L[所属 LogicThread / Lua VM] -->|异步 SQL 任务| M[MsgBus MySQL 队列]
    L -->|带 dbShard 的 Redis 任务| R[MsgBus Redis 分片队列]
    M --> D[DBThread MySQL 通道]
    R --> V[DBThread Redis 分片通道]
    D -->|DbResult / srcWorkerId / Actor epoch / ctx| Q[所属 worker 队列]
    V -->|DbResult / srcWorkerId / Actor epoch / ctx| Q
    L -->|timer.after/every/cancel 对应操作| T[TimerThread 操作队列 / 时间轮]
    T -->|到期消息 / timerId / Actor epoch| Q
    Q --> C[LogicThread 校验 Actor epoch]
    C -->|有效 DB 结果| B[Lua DB 回调；ctx=0 仅检查错误]
    C -->|有效定时器| F[Lua 定时回调或 SYS 定时任务]
    C -->|玩家已退出或世代过期| X[丢弃迟到回调]
    S[主线程收到 SIGUSR1] --> H[SceneServer::reloadLua]
    H -->|版本号 + 目录；广播| Q
    Q -->|重载消息| U[各 worker 在自己的线程重载 Lua]
```

DB/定时器线程不进入 Lua，也不直接操作 Player。Actor epoch 用于异步回调失效校验；clientEpoch 用于网关会话/视野绑定，两者职责不同。DB 任务入队不等于存档成功，热更新广播也不等于所有 worker 已成功加载。

### AOI 跨 worker 副本与恢复数据流图

```mermaid
flowchart TD
    P[Owner worker：PlayerService] -->|进场 / 移动 / 离开| A[本地 AoiService / 权威实体]
    A --> E[PublishHooks：AoiWire 编码事件]
    E --> S[WorkerPushSink::publishToWorkers]
    S -->|MSGTYPE_SERVICE；可信 senderWorkerId| Q[其他 worker 的 MsgBus 队列]
    Q --> R[LogicThread::onService / AoiServiceRouter]
    R -->|校验 sender、owner、method 与载荷| V[远端 AOI 副本]
    V -->|MOVE 暂存| T[AoiService::OnTick 应用移动]
    A -->|周期 OnTick| T
    V -->|发现需恢复的 owner| K[advanceAoiRecovery]
    K -->|AOI_OWNER_SNAPSHOT_REQ；周期重试| O[Owner worker：BuildOwnerSnapshot]
    O -->|AOI_OWNER_SNAPSHOT；入队失败待重试| R
    R -->|ApplyOwnerSnapshot| V
    T --> W[脏观察者 / 视野状态]
    W -->|ProcessAoiClientObserver| C[客户端差集 / 全量视野协议适配]
    C -->|WorkerPushSink::sendToClient| N[网络队列 → 网关 → 客户端]
```

当前 `LogicThread` 已注入 `AoiServiceRouter::PublishHooks`，并绑定 `ProcessAoiClientObserver`。跨 worker ENTRY/MOVE/LEAVE 与 owner 快照恢复通过**进程内消息总线**运行，不走网关 TCP，不属于跨场景进程迁移。恢复轮询间隔当前为 1000 ms，不能把请求入队视为恢复完成。客户端视野推送经 `WorkerPushSink` 进入网络队列；副本同步或队列接受仍不代表客户端收到。

## 内部消息处理流程

1. `main.cc` 在创建工作线程前阻塞 `SIGINT/SIGTERM/SIGUSR1`、忽略 `SIGPIPE`，读取两份配置并 `AttachZone()`，然后校验路由表及 protobuf 类型。
2. `SceneServer` 创建总线并加载地图，地图失败则不启动网络；随后启动网络、定时器、可选 DB 和逻辑 worker，最后反向连接网关。
3. `NetServer` 解码并校验绑定信封，经 `MsgBus` 按玩家/会话投递；ONLINE 创建/换绑 Actor，普通业务先查路由和已有绑定，再分派 C++ 或 Lua 处理。
4. 玩法处理通过网络队列回传结果；DB/定时器结果回投所属逻辑线程。
5. `SIGUSR1` 广播当前 `luaDir` 的脚本热更新；`SIGINT/SIGTERM` 经 `SceneServer::stop()` 按网络、逻辑、定时器、DB 的顺序有序停服。

## 配置、构建与运行

### 最小玩家验收链路

1. 客户端在网关执行 Auth；实际选定场景通道后，网关首次声明 ONLINE（仅建立 Player/会话绑定，不是角色加载成功确认）。
2. 客户端发送 `module=3, method=4`，body 为 `gs.EnterSceneReq`（空消息）。场景服 C++ 准入入口使用当前开发期默认 Player 数据，出生点 `(0,0,0)`，通过 PlayerService 建立 AOI 实体并注册观察者；返回 `gs.EnterSceneAck`，包含 entity_id、generation 和位置。重复请求保留当前位置，不重新分配世代。
3. 客户端发送 `module=3, method=2` 的 `gs.WalkReq`。Lua 校验请求起点等于权威坐标、移动距离不超过 100，再调用 `player.move`；C++ 验证地图边界并同步 AOI。错误 RetTip 沿原请求头和 seq 返回，不改变位置。
4. 客户端可以发送 `module=3, method=5` 的空 `gs.LogoutReq`，返回 `gs.LogoutAck`；Player 被清理，此客户端会话再发业务请求不会复活 Actor。重新 Auth 获得新 epoch 后可再次进入。客户端直接断开也通过网关 OFFLINE 完成同样的场景清理。

上述无 DB 最小链路不是生产登录系统。启用持久化时可从数据库加载角色快照并进行保存，见下文真实数据库验收；权威登录票据与正式账号链路不在此验收范围。AOI 观察者差集/全量推送已接入，但进场与移动成功不等于其他玩家已实际收到视野推送。

`SceneServer.mapPath` 可指定地图文件，默认 `map.yaml`，相对路径按场景配置文件目录解析。启动前加载地图并由 SceneServer 持有，向全部 worker 注入只读引用；地图加载失败不启动网络线程。

依赖升级后请新建构建目录，避免旧 Protobuf 生成文件与当前运行库混用。Lua 绑定使用 5.4/5.3 API，构建优先查找对应版本：

```bash
cmake -S Zone \
  -B Zone/build-acceptance -DSCENE_BUILD_TESTS=ON
cmake --build Zone/build-acceptance \
  --target sceneServer gatewayServer scene_tests -j4
ctest --test-dir Zone/build-acceptance \
  --output-on-failure -R '^(scene_tests|scene_smoke_invalid_route|gateway_real_scene)$'
```

`gateway_real_scene` 使用真实网关/场景服进程及标准库 Python 客户端，禁用 DB，覆盖进场、移动、错误回程、连接复用、主动退出与断开清理。

- 共享配置 `Zone/config/zoneConfig.yaml`：`zoneId`、数据库、网关地址、日志。
- 专属配置 `Zone/sceneServer/config/sceneConfig.yaml`：`sceneId/serviceId`、逻辑线程数、`luaDir`、可选监听及网络/定时器/AOI 参数。`net.listenEnable` 默认 `false`，Lua 脚本根目录默认 `./lua_script`。
- AOI 当前仅支持 `nineGrid`，`tickMs/cellSize` 必须大于零，`viewRange` 为地图坐标单位，`cellPerLogicThread` 必须为 `0`；十字链表参数不支持。共享项不能重复放入场景配置，未知配置键会报错。
- **网关地址：**共享配置默认 `gateways: 127.0.0.1:13145`，与网关 `privateConfig` 内网监听一致；`10000` 是客户端公网端口，不应用于场景服反向连接。跨机器部署须改成实际可达的内网地址，并确保 `zoneId` 一致、`serviceId=8` 位于网关 `allowedServices` 中。
- 示例 `ZoneServer.db.enable: true`；实际运行需要可用的 MySQL、Redis 及对应账号。无数据库联调可以按需禁用 DB 通道，但相关持久化功能不工作。

从场景服目录启动以使用相对路径的 `./lua_script` 和日志目录。默认共享配置 `./config/zoneConfig.yaml` **不在此目录**，所以务必提供第一个位置参数；第二个参数显式指定场景配置：

```bash
cmake -S Zone \
  -B Zone/build-readme -DSCENE_BUILD_TESTS=ON
cmake --build Zone/build-readme --target sceneServer -j 4
(
  cd Zone/sceneServer &&
  ../build-readme/bin/sceneServer \
    ../config/zoneConfig.yaml \
    ./config/sceneConfig.yaml
)
```

日志写入 `ZoneServer.log.dir` 指定的目录（相对路径基于工作目录）；发送 `SIGUSR1` 重载 `luaDir` 下脚本，`SIGTERM` 或 `SIGINT` 触发有序退出。

测试：

```bash
cmake --build Zone/build-readme -j 4
ctest --test-dir Zone/build-readme \
  --output-on-failure -R '^(scene_tests|scene_smoke_invalid_route|gateway_real_scene)$'
```

`scene_tests` 包含玩家绑定、AOI 路由/服务/副本/视野状态及 Lua 绑定等单元测试；`scene_smoke_invalid_route` 通过临时配置验证真实网络帧经过总线和逻辑线程后，无效路由不会创建 Actor；`gateway_real_scene` 验证真实玩家主链路。进程测试不证明外部 DB、可靠持久化或完整客户端视野协议可用。

### 真实数据库持久化验收

持久化验收不会默认加入 CTest，也不会使用 `Zone/config/zoneConfig.yaml` 中的数据库。它由脚本创建临时 MariaDB/Redis、回环端口、独立数据库和受限账号，结束时停止本次进程并删除临时数据。需要本机提供 MariaDB 初始化/服务端/客户端工具以及 Redis 服务端。

以下命令均以 `GameServer` 为根目录执行：

```bash
cmake -S Zone \
  -B Zone/build-acceptance
cmake --build Zone/build-acceptance \
  --target sceneServer gatewayServer persistence_dao_probe -j2
python3 Zone/sceneServer/tests/persistence_e2e.py \
  Zone/build-acceptance
```

成功摘要写入 `Zone/build-acceptance/persistence_e2e_summary.txt`。当前已通过的真实检查包括：DAO 加载/回滚/同版本重放/旧版本拒绝，非默认出生点加载，离线保存后立即重新绑定加载，在线周期快照，优雅停服最终保存，场景服重启恢复，以及复用网关连接时另一玩家断开后的独立持久化。

该验收覆盖正常时序和受控的队列/结果处理，不证明进程崩溃后内存任务不丢失、跨重启任务恢复、COMMIT 断链后的未知结果自动对账、跨网关接管、死锁不存在或所有 MySQL/MariaDB 版本兼容。若因 `SIGKILL` 或主机故障无法清理，应先确认残留进程属于本次临时目录，再手工清理，不能停止其他数据库实例。

文档路径以 `GameServer` 为根目录；各段构建与测试命令默认从根目录执行。运行示例在子 shell 中切换工作目录，退出后不改变当前终端目录。`luaDir` 和相对日志路径基于工作目录，`mapPath` 相对路径基于场景配置目录。

## 最新吞吐观测的归属与边界

2026-10-19 最近客户端等待优化轮，3G2S、512 × 20 RPS、30 秒成功吞吐 **10,228.37 RPS**、p99 **178.06 ms**、业务错误/unresolved 为 0；较早独立 60 秒轮为 **10,239.08 RPS**、p99 **83.59 ms**、零业务错误。它们说明两个 Scene 合计在特定移动/AOI 场景下具有短时万级端到端成功吞吐证据，不保证单 Scene、混合业务或长期稳定。

最新轮服务 CPU 0–9、客户端 CPU 10–15，仍有早段下探与追赶，最大调度滞后约 5.22 秒；与旧同绑核客户端的约 +73.2% 变化不能归于本节活跃索引，也不是严格冻结布局 A/B。capacity 模式未最终逐玩家核对 DB 快照；perf 收尾仍 exit=-15，不能用其不完整占比证明优化。数据和限制见[压测归档索引](../../docs/benchmark-results/README.md)。

LoginServer **待实现**，职责仅为账号认证、登录凭证、区服列表与角色归属查询。跨 worker AOI 不意味着不同 Scene 或不同 Zone 自动互通，详见[顶层区服关系](../../README.md)。
