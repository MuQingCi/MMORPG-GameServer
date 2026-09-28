# 场景服务器（sceneServer）

## 内部功能

- **面向网关**：主动连接区服共享配置中的网关内网地址，使用 `GateFrame` 握手、收发业务消息；本地直连监听默认关闭。
- **面向玩法**：按 `(module, method)` 路由 protobuf 请求，在玩家所属的逻辑线程中执行 Lua 处理器并生成响应或推送。
- **面向数据和定时任务**：通过消息总线异步处理 MySQL/Redis 任务、定时回调、玩家状态保存与 Lua 热更新。

## 职责边界

- 客户端只连接网关的 `ClientFrame` 端口；场景服处理服务间 `GateFrame`，不负责客户端鉴权、登录票据验证或公网接入。
- `NetServer` 持有网络连接与编解码，`LogicThread` 持有各自的 Lua VM 和玩家分片；数据库与定时器线程各司其职，跨线程只通过 `MsgBus` 投递消息。
- 按 `playerId` 路由玩家请求到所属 worker；没有玩家 ID 时按会话分配。无效的 `(module, method)` 在创建玩家 Actor 之前拒绝。
- 仅实现路由表中登记的玩法请求；没有 Lua 处理器的 C++ 入站路由当前尚未实现。`net.listenEnable` 仅供本地联调/工具，不应作为公网客户端入口。

## 与 Client 的消息流

场景服**不直接收发**客户端帧，客户端先经网关鉴权和转发：

客户端链路当前使用 `ClientFrame v2`（32 字节头、版本 2、12 字节角色名）；
场景服仍仅接收 `GateFrame v1`，不会解析客户端角色名。

```text
Client -- ClientFrame(Request) --> Gateway -- GateFrame --> SceneServer
Client <-- ClientFrame(Response/Push) <-- Gateway <-- GateFrame <-- SceneServer
```

网关从已鉴权会话写入 `playerId`、内部 `seq` 和目标服务号；场景服接收业务帧并回复到原连接。网关要求普通响应保持原请求的 `seq/playerId/module/method`，才能按回程路由还原客户端 `requestId`。详见 `GameServer/Zone/gatewayServer/README.md` 与 `GameServer/Zone/PROTOCOL.md`。

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

头部多字节整数为**网络序（大端）**；握手体里的 `zoneId`、`sceneId` 为**小端**。定义见 `GameServer/Zone/zone_common/base/proto.h`。玩法路由见 `GameServer/Zone/sceneServer/src-cpp/routeTable.cc`：当前入站 Lua 路由有 `PLAYER_MOVE`（`gs.WalkReq` → `gs.WalkAck`）与 `PLAYER_PATH`（`gs.PathReq` → `gs.PathAck`），还有仅出站的推送路由。

### 场景服将结果返回给客户端

```text
GateFrame --> NetServer::onFrame --> MsgBus（玩家/会话所属 worker）
          --> LogicThread::onNetMsg --> RouteTable::FindC2S
          --> LuaEnv::DispatchC2S --> MsgBus 网络发送队列
          --> NetServer::sendTo --> GateFrame --> Gateway --> ClientFrame
```

无效路由会返回 `gs.RetTip{code=1001,text="route not found"}` 且不创建 Actor。关联请求的错误响应沿用原请求的 `module/method/seq/playerId`，因此能通过网关回程路由并恢复客户端 `requestId`；客户端需在该请求的 Response body 中识别 `gs.RetTip`（当前服务间协议没有显式错误类别）。无关联请求的 RetTip 通知仍使用 SYS 模块/方法。

## 与 BackendServer 的消息流

场景服本身是网关的后端服务：启动后从 `ZoneServer.gateways` 读取地址，**反向连接网关内网端口**，发送 `GateFrame(module=SYS, method=kHandshake)`。握手体是 `serviceId(u8)+zoneId(u32 小端)+sceneId(u32 小端)`，共 9 B；出站 `srcServiceID` 取 `SceneServer.serviceId`，示例为 `8` (`ServerID::kScene_1`)。网关检查区服 ID 与白名单，注册前不接受业务帧；同服务号重新注册会替换旧连接。

```text
SceneServer NetServer -- 握手 GateFrame --> Gateway privateConfig 端口
Gateway -- 业务 GateFrame(dst=SceneServer.serviceId) --> NetServer
NetServer -- 响应/推送 GateFrame(dst=ServerID::kGateway) --> Gateway
```

网络层处理非阻塞收发、帧解码及缓冲/帧速率限制；当前没有完整的后端心跳或同服务号多实例路由能力。

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

主要接口：`SceneServer::start/stop` 控制生命周期，`reloadLua` 广播热更新，`stats` 输出各 worker、定时器与 DB 快照；`MsgBus::sendToWorkerByPlayerId/sendToNet/sendToDb` 执行跨线程投递。生命周期细节见 `GameServer/Zone/sceneServer/src-cpp/sceneServer.h`。

## 内部消息处理流程

1. `main.cc` 在创建工作线程前阻塞 `SIGINT/SIGTERM/SIGUSR1`、忽略 `SIGPIPE`，读取两份配置并 `AttachZone()`，然后校验路由表及 protobuf 类型。
2. `SceneServer` 创建总线，依次启动网络、定时器、可选 DB 线程及逻辑 worker，最后反向连接网关。
3. `NetServer` 解码 `GateFrame`，经 `MsgBus` 按玩家/会话投递；`LogicThread` 先查路由，合法请求才创建 Actor 并调用 Lua。
4. 玩法处理通过网络队列回传结果；DB/定时器结果回投所属逻辑线程。
5. `SIGUSR1` 广播当前 `luaDir` 的脚本热更新；`SIGINT/SIGTERM` 经 `SceneServer::stop()` 按网络、逻辑、定时器、DB 的顺序有序停服。

## 配置、构建与运行

- 共享配置 `GameServer/Zone/config/zoneConfig.yaml`：`zoneId`、数据库、网关地址、日志。
- 专属配置 `GameServer/Zone/sceneServer/config/sceneConfig.yaml`：`sceneId/serviceId`、逻辑线程数、`luaDir`、可选监听及网络/定时器限额。`net.listenEnable` 默认 `false`，Lua 脚本根目录默认 `./lua_script`。
- **网关地址：**共享配置默认 `gateways: 127.0.0.1:13145`，与网关 `privateConfig` 内网监听一致；`10000` 是客户端公网端口，不应用于场景服反向连接。跨机器部署须改成实际可达的内网地址，并确保 `zoneId` 一致、`serviceId=8` 位于网关 `allowedServices` 中。
- 示例 `ZoneServer.db.enable: true`；实际运行需要可用的 MySQL、Redis 及对应账号。无数据库联调可以按需禁用 DB 通道，但相关持久化功能不工作。

从场景服目录启动以使用相对路径的 `./lua_script` 和日志目录。默认共享配置 `./config/zoneConfig.yaml` **不在此目录**，所以务必提供第一个位置参数；第二个参数显式指定场景配置：

```bash
cmake -S GameServer/Zone -B GameServer/Zone/build -DSCENE_BUILD_TESTS=ON
cmake --build GameServer/Zone/build --target sceneServer -j 4
cd GameServer/Zone/sceneServer
GameServer/Zone/build/bin/sceneServer \
  GameServer/Zone/config/zoneConfig.yaml \
  GameServer/Zone/sceneServer/config/sceneConfig.yaml
```

日志写入 `ZoneServer.log.dir` 指定的目录（相对路径基于工作目录）；发送 `SIGUSR1` 重载 `luaDir` 下脚本，`SIGTERM` 或 `SIGINT` 触发有序退出。

测试：

```bash
cmake --build GameServer/Zone/build -j 4
ctest --test-dir GameServer/Zone/build --output-on-failure -R '^scene_'
```

包括 `scene_tests` 与 `scene_smoke_invalid_route`；后者通过临时配置验证真实网络帧经过总线和逻辑线程后，无效路由不会创建 Actor。
