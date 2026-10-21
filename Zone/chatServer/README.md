# 聊天服务器

## 目录

- [本服务器简介](#本服务器简介)
- [构建与运行](#构建与运行)
- [功能与职责边界](#功能与职责边界)
  - [功能示例](#功能示例)
  - [职责边界](#职责边界)
- [消息格式](#消息格式)
- [私聊处理流程](#私聊处理流程)
- [全区喊话与管理员限制](#全区喊话与管理员限制)
- [测试与适用范围](#测试与适用范围)

## 本服务器简介

聊天服提供同一区服内的在线私聊和全区喊话。玩家可以连接不同网关、处于不同 Scene；管理员广播尚不可用。

服务间复用 `GateFrame`（32 字节头，魔数 `0xC1EA`，版本 1），没有额外的聊天帧头。协议见[线协议](../PROTOCOL.md)。路径以 `GameServer` 为根目录，命令默认在根目录执行。

## 构建与运行

依赖与公共构建设置见[主 README](../../README.md#各子服务器的构建命令)。从仓库根目录构建网关和聊天服：

```bash
cmake -S Zone -B out/zone -DSCENE_BUILD_TESTS=ON
cmake --build out/zone --target gatewayServer chatServer -j 2
```

先按[网关说明](../gatewayServer/README.md#构建与运行)启动网关，在 `gateway.privateConfig.allowedServices` 中保留 `8` 并加入聊天服务号 `3`。如需同时运行 Global，再加入 `4`。随后启动聊天服：

```bash
out/zone/bin/chatServer 127.0.0.1 13145 1
```

聊天服支持追加同一区服的多个网关。每组三个参数依次为网关内网 IPv4 地址、端口、区服 ID，例如 `chatServer 127.0.0.1 13145 1 127.0.0.1 13146 1`。全局服仍只连接一个网关。

部署时注意：

- 所有网关必须允许服务号 3，客户端登录时，所在网关应已注册聊天服。
- 重复地址/端口或不同区服 ID 会导致启动失败。
- 任一网关链路断开，聊天进程都会退出，需要部署系统重启；目前没有自动重连或离线消息。
- 广播只发给已配置网关，其他网关上的客户端不会收到。

## 功能与职责边界

聊天服只处理当前在线会话的消息，不存储玩家数据或离线消息。

- 同一区服在线私聊：可跨网关，不依赖玩家属于哪个 Scene。
- 同一区服普通玩家全区喊话：向已配置网关扇出。
- 管理员广播仅预留方法，当前不可用；不能通过开发期票据取得管理员权限。

### 功能示例
- 私聊：Scene 1 中玩家 12345 向 Scene 3 中玩家 41231 发送“hello”。目标在线时会收到消息，展示名取自发送方提交的角色名，例如“PlayerName: hello”。
- 喊话：在线玩家发送消息，已配置网关上的已鉴权会话接收推送。范围仅限同一 Zone，不保证可靠送达。

### 职责边界
- 当前聊天服不操作玩家数据；由网关会话校验发送人 ID，聊天服按目标玩家 ID、名字和消息体构造 `GateFrame` 推送。
- 聊天服不查询玩家在哪个网关，而是向所有已配置网关发送推送，由各网关筛选本地会话。
- 规划中的 `gatewaySessionTable`（gatewayId → 玩家集合）和 `pGatewayTable`（playerId → gatewayId）尚未实现。

## 消息格式

ChatServer ↔ GatewayServer 使用 32 字节 `GateFrame`（魔数 `0xC1EA`，版本 1）。
头部含 `module/method/seq/playerId/totalLen/srcServiceID/dstServiceID`，服务号为
`ServerID::kChat=3`；请求与响应使用相同 `module=100/method=1` 和非零 `seq`，
推送使用 `seq=0`，头部 `playerId` 为目标玩家。字节偏移见
[线协议](../PROTOCOL.md)。

## 私聊处理流程

网关提供可信的发送方 ID，聊天服检查消息后向各网关推送，由网关筛选目标会话。

1. 客户端鉴权后发送 ClientFrame **v2** Request：32 字节头，版本 2，module=100，method=1，requestId≠0。头部角色名为非空 UTF-8，最多 12 字节，右侧补零。
2. body 为 `目标 playerId(u64，大端) + UTF-8 消息(1..1024 字节)`，文本不允许控制字符。网关从会话取得真实发送人 ID，将名字插入服务间请求体：`目标 playerId(u64 大端) + 12B角色名 + 消息`。
3. 聊天服校验后向发送人返回 Response，保留 module/method/seq，body 为 `accepted` 或错误描述；网关恢复客户端 requestId。
4. 聊天服向网关发送 Push，body 为 `发送人 ID + 12B角色名 + 原消息`。网关筛选本地目标会话，再转成 requestId=0 的客户端推送：角色名放在帧头，body 为 `发送人 playerId(u64，大端) + 原消息`。

缺少目标 ID 时，网关直接返回 `kError`。目标 ID 为零、给自己私聊、文本为空、超长或编码非法时，聊天服返回文本错误，保留原 `module/method/seq`。未知方法返回 `unsupported method`，不生成推送。

`accepted` 只表示响应和各网关推送进入了聊天服的出站队列，不表示对方在线、已收到或已读。目标不在线时，各网关会丢弃推送。

## 全区喊话与管理员限制

普通玩家喊话向同一区服的已配置网关广播，发送者本人也会收到。

1. 鉴权客户端发送 `module=100,method=2`，body 为 1..1024 字节的 UTF-8 文本，不允许控制字符，头部必须有非空展示名。
2. 网关从会话注入可信发送方 ID，将 12B 展示名放在服务间请求体开头。
3. 聊天服校验后回复同请求号的 `accepted`，并向所有已配置网关发送 `seq=0,playerId=0` 的广播，body 为 `senderId(u64 BE)+name(12B)+text`。
4. 网关向本地已鉴权会话发送 `Push(module=100,method=2,requestId=0)`。ClientFrame 头部放展示名，body 为 `senderId(u64 BE)+text`，发送者本人也会收到。

无效文本返回 `invalid chat message`，不推送。

`module=100,method=3` 为预留的管理员广播方法，当前统一回复 `admin broadcast unavailable`，**不会广播**。开发期共享票据不是管理员身份凭证，不能通过客户端角色名、玩家 ID 或消息体字段伪造权限。
**名字未经账号/角色服务验证，可被发送人伪造，只能用于展示，不得用于鉴权或权限判断**。

## 测试与适用范围

`gateway_chat_global` 检查网关、聊天服和全局服交互。先构建所需目标，再运行该用例：

```bash
cmake --build out/zone --target gatewayServer chatServer globalServer -j 2
ctest --test-dir out/zone -R '^gateway_chat_global$' --output-on-failure
```

这里提供测试入口，不表示本次文档整理已执行或通过该用例。目前没有独立的聊天吞吐和长时间稳定性测量。

“跨场景”只指同一区服内的聊天，不是跨 Zone。一个聊天进程不能连接不同区服 ID 的网关。LoginServer 尚未实现，规划负责账号认证、登录凭证、区服列表和角色归属查询。

[项目架构](../../README.md)中的最新 3G2S 数据来自移动/AOI 场景，不代表聊天吞吐或跨区能力。
