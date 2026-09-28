# ClearMoon GameServer

基于 C++20 的 MMORPG 分区分服服务端实验项目。当前可构建、联调的部分位于 `GameServer/Zone`：网关接入客户端，场景服处理部分玩法，聊天服处理在线私聊/喊话，全局服提供 Ping/Echo 接入验证。**这是开发与联调用底座，不是可直接上线的完整游戏后端。**

## 当前架构与实现范围

```text
客户端 -- ClientFrame v2 --> 网关公网端口 (默认 127.0.0.1:10000)
                              | 鉴权、会话、服务注册、请求/推送路由
                              | GateFrame v1 (内网端口默认 127.0.0.1:13145)
                +-------------+-------------+
                |             |             |
             场景服        聊天服        全局服
          Lua/逻辑线程    在线聊天       Ping/Echo
          MySQL/Redis*   (可连多网关)    (单网关)
```

\* 场景服的 DB 通道取决于构建依赖、配置和实际可用的数据库服务。

| 组件 | 当前能力 | 尚未实现或不保证 |
| --- | --- | --- |
| 网关 | 独立公网/内网监听；客户端会话、开发期票据校验；后端握手注册；请求响应关联与本地玩家推送 | 正式登录凭证、TLS/后端密码学认证、主动健康探测、信号驱动的优雅停服 |
| 场景服 | 按玩家分片的逻辑线程、Lua/Protobuf 路由；当前入站玩法主要为移动和寻路；可选 MySQL/Redis 通道 | 完整玩法及按 `sceneId` 的同服务号多实例路由 |
| 聊天服 | 单网关或多网关在线私聊、普通玩家全区喊话；向配置的网关扇出，由网关筛选本地会话 | 离线消息、可靠送达、自动重连、可信管理员广播 |
| 全局服 | 原样回显 Ping/Echo 的二进制消息体 | 公会、全区状态及持久化等实际全局业务；自动重连 |

两套协议**不能混用**：客户端到网关是 `ClientFrame v2`（魔数 `0xC1EB`），后端到网关是 `GateFrame v1`（魔数 `0xC1EA`）。详细字段、业务方法和限制参见 `GameServer/Zone/PROTOCOL.md`。

## 目录导航

- `GameServer/Zone/CMakeLists.txt`：当前区服的 CMake 入口。
- `GameServer/Zone/gatewayServer/README.md`：网关会话、双端口、服务注册和转发。
- `GameServer/Zone/sceneServer/README.md`：场景服路由、脚本、DB 与运行配置。
- `GameServer/Zone/chatServer/README.md`：聊天协议、跨网关联调与限制。
- `GameServer/Zone/globalServer/README.md`：全局服现有能力。
- `GameServer/Zone/zone_common`：共享协议、缓冲、配置和日志代码。
- `GameServer/docs`：设计、评审及演进记录；**以现行源码和协议文档为准**。
- `GameServer/LoginServer`：尚未接入当前区服构建的目录，不能把网关开发期票据视为已实现登录系统。

根目录 `GameServer/docker-compose.yml` **仅提供 Kafka**，不启动网关、场景服、MySQL 或 Redis；不能将 `docker compose up` 当作本项目一键启动命令。

## 构建和测试

环境：Linux、支持 C++20 的编译器、CMake ≥ 3.20、Protobuf 编译器和开发库、Lua 开发库、pthread。场景服构建时需要 Lua；MySQL/MariaDB 客户端开发库及 Valkey/hiredis 客户端开发库为可选依赖，缺少时相应 DB 通道会禁用。端到端测试还使用 Bash 和 Python 3（Python 测试只用标准库）。

```bash
cmake -S GameServer/Zone \
      -B GameServer/Zone/build \
      -DSCENE_BUILD_TESTS=ON
cmake --build GameServer/Zone/build -j 4
ctest --test-dir GameServer/Zone/build --output-on-failure
```

构建产物在 `GameServer/Zone/build/bin`。CTest 包含单元测试及真实进程联调：网关—场景服、单/双网关聊天和全局服 Echo。端到端测试通过**临时配置与本地进程**运行，不验证外部 MySQL/Redis 或默认配置下的完整业务上线流程。

## 本机运行与配置

默认示例保留两个独立端口：网关公网 `127.0.0.1:10000`、内网 `127.0.0.1:13145`。`GameServer/Zone/config/zoneConfig.yaml` 中的 `ZoneServer.gateways` 已指向**内网**端口。区服 ID、网关内网地址及后端服务号白名单必须一致；默认 `GameServer/Zone/gatewayServer/config/gatewayConfig.yaml` 的 `allowedServices` **仅包含场景服 8**，运行聊天服/全局服之前需分别加入 `3`/`4`（以 YAML 分行列表填写）。

请在不同终端分别执行以下命令。运行目录影响网关的默认配置路径、场景服的 `luaDir` 及日志位置：

```bash
# 终端 1：网关
cd GameServer/Zone/gatewayServer
   GameServer/Zone/build/bin/gatewayServer \
   GameServer/Zone/gatewayServer/config/gatewayConfig.yaml

# 终端 2：场景服（请先确认数据库配置和依赖）
cd GameServer/Zone/sceneServer
   GameServer/Zone/build/bin/sceneServer \
   GameServer/Zone/config/zoneConfig.yaml \
   GameServer/Zone/sceneServer/config/sceneConfig.yaml

# 终端 3、4：可选；需先在网关 allowedServices 中允许服务号 3、4
GameServer/Zone/build/bin/chatServer 127.0.0.1 13145 1
GameServer/Zone/build/bin/globalServer 127.0.0.1 13145 1
```

场景服默认启用 DB，并指向示例本机 MySQL/Redis 地址；实际部署前须提供服务和适配凭证。无需 DB 的联调可以在共享配置中禁用 DB，但持久化功能不可用。聊天服可追加更多组 `<网关内网 IPv4> <端口> <同一区服 ID>` 参数连接多个网关，参见聊天服文档；全局服目前只连接一个网关。示例中的 `127.0.0.1` 只适用于同一主机；跨主机/容器需配置可达的内网地址并限制后端端口访问。

## 安全与交付边界

- 网关当前 `clientToken` 是**所有客户端共享的开发期票据**，并非账号登录或按玩家签发的安全令牌；客户端提交的角色名只用于展示，不可用于权限判断。
- 管理员广播方法目前明确拒绝；普通玩家喊话是联调能力。聊天的 `accepted` 只表示推送已入聊天服出站队列，不保证目标在线、收到或已读。
- 后端握手未提供加密认证；单个服务号的场景服节点尚无基于 `sceneId` 的多实例选路。生产部署还需要可信鉴权、传输保护、健康检查、故障恢复、可靠消息和运维编排。
