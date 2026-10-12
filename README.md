# ClearMoon GameServer

基于 **C++20、Lua 与 Protobuf** 的 MMORPG 分区分服服务端实验项目。当前区服构建入口位于 `Zone/CMakeLists.txt`：网关接入客户端，场景服处理玩家绑定、进场/移动/退出和 AOI 内部状态，聊天服处理在线私聊/喊话，全局服提供 Ping/Echo 接入验证。**这是开发与联调用底座，不是可直接上线的完整游戏后端。**

## 当前架构与实现范围

```text
客户端 -- ClientFrame --> 网关公网端口 (默认 127.0.0.1:10000)
                              | 鉴权、会话、服务注册、请求/推送路由
                              | GateFrame (内网端口默认 127.0.0.1:13145)
                +-------------+-------------+
                |             |             |
             场景服        聊天服        全局服
           Lua/逻辑线程    在线聊天       Ping/Echo
           玩家绑定/AOI
          MySQL/Redis*   (可连多网关)    (单网关)
```

\* 场景服的 DB 通道取决于构建依赖、配置和实际可用的数据库服务。

后端启动后**主动连接网关内网端口**，不是由网关连接后端监听端口；场景服本地直连监听默认关闭。“公网端口”表示接入职责，默认回环地址并未对外开放。

| 组件 | 当前能力 |
| --- | --- |
| 网关 | 独立公网/内网监听；客户端会话、开发期共享票据校验；后端握手注册；请求响应关联与本地玩家推送 |
| 场景服 | 可信玩家绑定；按玩家分片的逻辑线程、独立 Lua VM；C++ 进场/退出、Lua 移动与示例路径；地图校验、九宫格 AOI 本地状态与跨 worker 事件；可选 MySQL/Redis 通道 |
| 聊天服 | 单网关或多网关在线私聊、普通玩家全区喊话；向配置的网关扇出，由网关筛选本地会话 |
| 全局服 | 原样回显 Ping/Echo 的二进制消息体 |

各服务尚未实现或未闭环的能力统一见 [规划实现](#规划实现尚未落地的架构设计)。

两套协议**不能混用**：客户端到网关是 `ClientFrame`（32 B 头，魔数 `0xC1EB`，整帧上限 64 KiB），后端到网关是 `GateFrame`（32 B 头，魔数 `0xC1EA`，整帧上限 10 MiB）。头部多字节整数为网络序；控制体、握手体与业务体应按各自定义解析。详细字段和业务限制参见 `Zone/PROTOCOL.md`；场景新增入口另见源码路由表。

### 场景服线程与 AOI 边界

- 网络线程独占连接、编解码和网关玩家绑定索引，不直接访问 Player 或 AOI。
- 每个逻辑 worker 持有自己的 Player、Lua VM 与 AOI 服务；网络、DB 和定时器结果通过 `MsgBus` 投递。
- 时间轮由定时器线程处理；MySQL/Redis 是独立 DB 通道，Redis 可配置多实例逻辑分片。
- 当前 AOI 仅支持 `nineGrid`，已具备实体生命周期、移动、副本事件/快照处理及观察者状态等内部机制，使用每个 worker 全场景副本方案。
- 跨 worker 事件发布、owner 快照恢复和观察者基线接口（`AoiViewSet`）已经接通；但客户端视野推送 hook（`processObserver`）尚未绑定，进场/移动成功只说明本地权威实体同步成功，不代表其他 worker 或玩家已收到视野更新。未闭环部分见 [规划实现](#规划实现尚未落地的架构设计)。

## 目录导航

- `Zone/CMakeLists.txt`：区服 CMake 入口，构建四个服务。
- `Zone/proto/gameProto.proto`：场景玩法消息定义，构建时自动生成 C++ 代码。
- `Zone/gatewayServer/README.md`：网关会话、双端口、服务注册和转发。
- `Zone/sceneServer/README.md`：玩家绑定、进场验收、AOI、脚本和 DB。
- `Zone/sceneServer/src-cpp/routeTable.cc`：当前实际玩法路由表，并非从示例路由 YAML 动态加载。
- `Zone/sceneServer/lua_script`：Lua 玩法与 DB/定时器示例。
- `Zone/chatServer/README.md`：聊天协议、跨网关联调与限制。
- `Zone/globalServer/README.md`：全局服现有能力。
- `Zone/zone_common`：共享协议、玩家信封、缓冲、配置、日志和最小服务运行时。
- `docs`：设计、评审及演进记录。
- `sql/`：区服建表脚本（`001_player.sql`、`002_inventory.sql`）。
- `Enviromation.md`：开发机环境记录，不代表最低版本要求。

根目录 `docker-compose.yml` **仅提供 Kafka**，不启动网关、场景服、MySQL 或 Redis；Kafka 不是当前区服构建必需依赖，不能将 `docker compose up` 当作项目一键启动命令。

## 构建和测试

环境：Linux、支持 C++20 的编译器、CMake ≥ 3.20、Protobuf 编译器和开发库、Lua 开发库、pthread。当前 CMake 优先查找 Lua 5.4/5.3，未找到时配置失败，不会禁用脚本层继续构建。MySQL/MariaDB 客户端开发库及 Valkey/hiredis 客户端开发库为可选依赖，缺少时相应 DB 通道会禁用。建议提供 `pkg-config`，用于发现 Lua 与 Protobuf 传递链接依赖。

端到端测试使用 Bash 和 Python 3（Python 客户端只用标准库）；缺少 Python 时部分用例不会注册。升级 Protobuf 后建议新建构建目录重新生成协议，避免旧生成文件与新运行库混用。

```bash
cmake -S Zone \
      -B Zone/build-readme \
      -DSCENE_BUILD_TESTS=ON
cmake --build Zone/build-readme -j 4
ctest --test-dir Zone/build-readme --output-on-failure
```

四个服务产物位于 `Zone/build-readme/bin`。默认构建类型为 `Debug`，可配置 `-DCMAKE_BUILD_TYPE=Release`。`SCENE_BUILD_TESTS` 默认为 `ON`；构建同时生成 `compile_commands.json`。

| CTest 用例 | 验证内容 |
| --- | --- |
| `scene_tests` | 缓冲、帧、消息总线、定时器、配置、Protobuf/Lua、Redis key、逻辑线程、网关绑定及 AOI |
| `scene_smoke_invalid_route` | 真实场景进程收到无效路由后不创建 Actor |
| `gateway_tests` | 客户端帧、配置、会话/服务/回程路由及网络基础组件 |
| `gateway_smoke` | 真实网关进程启动和双端口监听 |
| `gateway_e2e` | 真实网关、模拟后端与客户端的鉴权、转发、回程和协议隔离 |
| `gateway_real_scene` | 真实网关/场景服的进场、移动、错误回程、连接复用、退出与断开清理 |
| `gateway_chat_global` | 单/双网关聊天与全局 Echo 联调 |

端到端测试使用**临时配置与本地进程**，真实场景主链路禁用 DB，不验证外部 MySQL/Redis 或默认配置的持久化成功。完整构建后，可仅验收场景主链路：

```bash
ctest --test-dir Zone/build-readme \
      --output-on-failure \
      -R '^(scene_tests|scene_smoke_invalid_route|gateway_real_scene)$'
```

本次文档更新在独立构建目录完成完整构建，CTest **7 项中 6 项通过**，包括真实场景主链路和聊天/全局联调。`gateway_e2e` 当前失败：其模拟场景后端在 Auth 后直接将读到的帧断言为移动请求，尚未处理新加入的 ONLINE 通知；从测试代码与断言表现判断，这很可能是测试适配问题，仍需单独修复验证。本文不宣称全量测试已通过。

## 本机运行与配置

默认示例保留两个独立端口：网关客户端入口 `127.0.0.1:10000`、后端入口 `127.0.0.1:13145`。默认区服 `zoneId=1`、网关 `gatewayId=1`、场景 `sceneId=1/serviceId=8`，逻辑 worker 数量为 4。

| 配置文件路径（相对 GameServer 根目录） | 内容 |
| --- | --- |
| `Zone/gatewayServer/config/gatewayConfig.yaml` | 网关身份、双端口、票据、白名单和路由限额 |
| `Zone/config/zoneConfig.yaml` | 区服 ID、数据库、网关内网地址及场景日志 |
| `Zone/sceneServer/config/sceneConfig.yaml` | 场景/服务 ID、Lua、worker、地图、网络、时间轮和 AOI |
| `Zone/sceneServer/config/map.yaml` | 默认场景地图 |

启动前确认区服 ID、内网地址与后端白名单一致。`ZoneServer.gateways` 默认已指向内网端口；网关 `allowedServices` **仅包含场景服 8**，运行聊天/全局服前需加入 `3`/`4`，保留 `8`。轻量解析器不支持行内列表，须填写：

```yaml
allowedServices:
    - 8
    - 3
    - 4
```

场景配置会拒绝未知字段及已经迁移的共享项，区服 ID、DB、网关地址和日志不能重复放入 `SceneServer`。AOI 当前仅允许 `aoiKind: nineGrid`，`tickMs/cellSize` 必须大于零，`cellPerLogicThread` 必须为 `0`；`viewRange` 是地图坐标单位，不是格数，十字链表参数尚不支持。

请在不同终端分别执行以下命令。运行目录影响网关的默认配置路径、场景服的 `luaDir` 及日志位置：

```bash
# 终端 1：网关
(
  cd Zone/gatewayServer &&
  ../build-readme/bin/gatewayServer ./config/gatewayConfig.yaml
)

# 终端 2：场景服（请先确认数据库配置和依赖）
(
  cd Zone/sceneServer &&
  ../build-readme/bin/sceneServer \
    ../config/zoneConfig.yaml \
    ./config/sceneConfig.yaml
)

# 终端 3、4：可选；需先在网关 allowedServices 中允许服务号 3、4
Zone/build-readme/bin/chatServer 127.0.0.1 13145 1
Zone/build-readme/bin/globalServer 127.0.0.1 13145 1
```

场景服默认启用 DB，并指向示例本机 MySQL/Redis 地址；实际部署前须提供服务和适配凭证。无需 DB 的联调可以在共享配置中禁用 DB，但持久化功能不可用。聊天服可追加更多组 `<网关内网 IPv4> <端口> <同一区服 ID>` 参数连接多个网关，参见聊天服文档；全局服目前只连接一个网关。示例中的 `127.0.0.1` 只适用于同一主机；跨主机/容器需配置可达的内网地址并限制后端端口访问。

默认 MySQL 为 `127.0.0.1:3306`、数据库 `game`，Redis 为 `127.0.0.1:6379`。无 DB 联调可将 `ZoneServer.db.enable` 设为 `false`。聊天服任意网关链路断开都会导致进程退出，当前没有自动重连。

场景服参数顺序为“共享配置、场景配置”。即使显式指定配置文件路径，`luaDir: ./lua_script` 和相对日志目录仍基于**进程工作目录**；`mapPath` 相对路径则基于**场景配置文件目录**，默认 `map.yaml`。地图加载失败时不会启动网络线程。

## 最小玩家验收链路

网关发给场景服的玩家消息使用 `Zone/zone_common/base/gatewayPlayer.h` 定义的 **32 B 版本化绑定信封**，随后才是玩法载荷。网关与场景服须同步升级；旧裸 body 玩家请求不再接受。专用 `module=200` 的 ONLINE/OFFLINE 不允许公网客户端调用。

1. **Auth**：客户端发送 `Control(module=1, method=1)`，body 为 `playerId(u64 LE)+ticket(u64 LE)`，默认 ticket 为 `998877`。成功后网关建立会话并向场景服发布 ONLINE；只建立 Player/绑定，不表示角色加载完成，不自动进入 AOI。
2. **进场**：发送 `Request(module=3, method=4)`，body 为 `gs.EnterSceneReq`（空消息）。当前出生点 `(0,0,0)`，返回 `gs.EnterSceneAck`，包含实体 ID、generation 和位置；重复请求保留当前位置，不重新分配世代。
3. **移动**：发送 `Request(module=3, method=2)`，body 为 `gs.WalkReq`。Lua 校验起点等于权威坐标、曼哈顿距离不超过 100；C++ 校验地图边界并同步 AOI，成功返回 `gs.WalkAck`。
4. **路径示例**：`module=3, method=3` 使用 `gs.PathReq/gs.PathAck`。当前 Lua 返回固定示例路径，**不是真实寻路算法**。
5. **退出**：发送 `Request(module=3, method=5)`，body 为 `gs.LogoutReq`（空消息），返回 `gs.LogoutAck` 并清理 Player。原会话继续发业务请求不会复活 Actor；重新 Auth 获取新 epoch 后才可再次进入。客户端断开通过网关 OFFLINE 清理。

业务 `requestId` 必须非零。网关使用内部 `seq` 关联回程，后端普通响应须保留原请求的 `seq/playerId/module/method`。无效路由返回 `gs.RetTip` 且不创建 Actor；普通合法业务也不能隐式创建或换绑玩家。

`gateway_real_scene` 可直接验证该主链路，无需手写客户端。聊天使用 `module=100`，全局 Echo 使用 `module=101, method=1`；具体 body 格式见协议及对应服务文档。

## 日志、热更新与排障

- 网关日志固定写入启动目录的 `./logs`，场景日志由 `ZoneServer.log.dir/name/level` 配置。
- 场景服 `SIGUSR1` 向 worker 广播当前 `luaDir` 脚本重载；`SIGINT/SIGTERM` 触发有序停服。
- 网关目前没有进程信号驱动的优雅停服，不应将直接终止视为在途消息已可靠完成。

| 现象 | 优先检查 |
| --- | --- |
| CMake 找不到 Lua/Protobuf | 开发头文件、库、`protoc` 和 CMake 查找结果；Lua 是必需依赖 |
| Protobuf 编译/链接错误 | 生成文件与运行库是否匹配；升级后使用新目录，确认 `pkg-config` 提供传递依赖 |
| 配置/脚本打不开 | 位置参数及工作目录，特别是相对 `luaDir` |
| 地图加载失败 | `mapPath`、配置目录、地图内容和 AOI `cellSize` |
| 后端无法注册/业务不可用 | 网关先启动、连接内网端口、区服 ID 和白名单一致 |
| DB 不可用 | 客户端库发现结果、`db.enable`、服务可达性、账号和数据库 `game` |
| 移动失败或退出后请求失败 | 已 Auth、绑定有效、已进场；退出后需重新 Auth |
| 移动成功但无视野推送 | 客户端视野推送 hook（`processObserver`）尚未绑定，见 [规划实现](#规划实现尚未落地的架构设计) |

## 规划实现（尚未落地的架构设计）

本节汇总架构设计中**尚未实现或未闭环**的能力，均为设计目标，不代表当前已有行为。任务优先级、依赖与验收门槛以 [`docs/顺序实现清单.md`](docs/顺序实现清单.md) 为准；各专题设计见 `docs/` 下对应文档。

### 可信鉴权与角色准入

- **可信身份**：网关当前只比较共享 `clientToken`，不验证角色归属；绑定信封（`zone_common/base/gatewayPlayer.h`）不含 `accountId`。规划引入绑定可信 accountId/playerId/zoneId 的凭证、异步网关鉴权与信封版本化扩展。
- **角色加载与准入**：场景当前用开发期默认 Player 数据进场，出生点固定 `(0,0,0)`，无角色数据库加载与账号归属校验，`AuthAck` 仍只代表网关绑定。规划接入角色 DAO 一致加载与 `Ready` 准入门。
- **传输安全**：网关无 TLS、后端握手为明文；生产需要入区凭证签发与校验、后端 mTLS/可信网络与重放防护。

### 存档与数据一致性

- **已落地**：异步 DB 通道、玩家周期快照 UPSERT、Actor epoch 回调过滤；本轮补充 `PendingDbTasks`（待投递任务去重与重试）与 `DbResultLedger`（结果待回投与投递重试），`MsgBus` 投递返回 `DispatchResult` 以暴露背压。
- **未闭环**：周期保存**入队成功即清 dirty**、退出不等存档 ACK、快照 UPSERT 无 `save_version` 条件、背包/仓库保存与容器未接入、`DbTask` 为单条 SQL 无事务批次、离线保存与可靠结果回投未保证。规划引入保存协调器、版本化任务、事务执行器、业务操作 ID 幂等与关服排空确认（详见 `docs/逻辑线程与DB层的数据一致性实现文档.md`）。

### AOI 与客户端视野

- **已落地**：九宫格 AOI、实体生命周期、跨 worker ENTRY/MOVE/LEAVE、owner 快照请求/恢复、观察者基线接口（`AoiViewSet`）。
- **未实现**：`processObserver` 未绑定，缺“差集计算 → Protobuf → 分片 → 入队 → 基线提交 → 网络失败反馈 → 客户端 `resync`”的完整运行链路；客户端视野增量/快照/重同步入站协议未形成；`gatewayBootId`、可靠在线集合与跨进程接管未实现。

### 网关健壮性与多实例

- **未实现**：正式身份认证、加密传输、主动健康探测、信号驱动优雅停服、断链恢复与在线集合对齐；路由 TTL 只清理路由，不是可靠超时回包。
- **未实现**：按 `sceneId` 的同服务号多实例选路（握手 `sceneId` 当前只记录）、owner lease、实例启动身份与跨网关权威接管。

### 聊天服与全局服

- 聊天服未实现离线消息、可靠送达确认、自动重连、跨网关会话表（`gatewaySessionTable`/`pGatewayTable`）与可信管理员广播；当前向所有已配置网关扇出，由网关本地筛选。
- 全局服仅 Ping/Echo，未实现公会、全区状态、数据访问等业务及自动重连。

### Redis 安全与工程化

- Redis 前缀校验不是完整安全边界：`FLUSHALL`/`FLUSHDB`/`CONFIG` 等危险无 key 命令与未声明的多 key/`EVAL` 访问未被完整拦截，规划增加命令 allowlist、参数形状校验并配合部署 ACL/端点隔离。
- `Zone/zone_common/CMakeLists.txt` 为空，公共源文件由各 target 分别编译，尚未建立统一公共库；规划抽独立公共 CMake target 并统一 include 边界。

### 玩法

- 真实寻路（替换固定 `PathReq` 示例）、战斗、怪物/NPC 生命周期与掉落持久化尚未实现。

### 进行中的修复

- `gateway_e2e` 的模拟后端尚未适配新增的 ONLINE 帧与绑定信封，当前失败；需单独修复测试而不删除安全断言。

## 安全与交付边界

- 网关当前 `clientToken` 是**所有客户端共享的开发期票据**，并非账号登录或按玩家签发的安全令牌；客户端提交的角色名只用于展示，不可用于权限判断。
- 管理员广播方法目前明确拒绝；普通玩家喊话是联调能力。聊天的 `accepted` 只表示推送已入聊天服出站队列，不保证目标在线、收到或已读。
- Redis 多实例属于框架逻辑分片，不等同于 Redis Cluster；跨分片多 key 命令会拒绝。脚本应使用框架 key 构造接口满足区服/场景命名空间约束。
- 网络入队成功、TCP 发送成功、聊天 `accepted`、数据库入队与客户端收到是不同确认层级；AOI 内部状态与完整视野同步、DB 通道与可靠持久化不能混为一谈。

以上之外的生产化能力（可信鉴权、加密传输、可靠持久化、客户端视野协议、多实例选路、真实玩法等）见 [规划实现](#规划实现尚未落地的架构设计)。
