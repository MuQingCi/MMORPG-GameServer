# 聊天服务器

> 当前实现复用服务间 `GateFrame`（32 字节头，魔数 `0xC1EA`，版本 1），不另设聊天专用帧头。实现在线跨网关私聊和普通玩家全区喊话；管理员广播仍不可用。协议以 `GameServer/Zone/PROTOCOL.md` 为准。
## 内部支持功能
- 1.场景服内部私聊  
- 2.跨场景服私聊  
- 3.某一场景服内玩家对整个区级内部玩家喊话  
- 4.管理员对当前区广播消息  
> 3/4可以合并，由网关鉴权后传递玩家身份给聊天服务器，聊天服务器根据权限调用不同广播方法  

### 功能示例
- 1.跨场景服私聊  
    - 例如:场景服1中一位PlayerID为12345的玩家向场景服3中PlayerID为41231的玩家发送一句消息"hello",后者会接收到一条"PlayerName（12345对应的角色名）: hello"  
- 2.跨服广播 
    - 管理者定时地向各个场景服广播消息，各个场景服内部的每个玩家都能收到消息

## 职责边界
- 当前聊天服不操作玩家数据；由网关会话校验发送人 ID，聊天服按目标玩家 ID、名字和消息体构造 `GateFrame` 推送。
- 未实现跨网关会话表（不查询玩家位置，而是向所有已配置网关扇出）：


> 规划：`gatewaySessionTable`（gatewayId → 玩家集合）、`pGatewayTable`（playerId → gatewayId）；这些索引当前均不存在。聊天服按启动参数同时连接多个网关，由各网关按本地在线会话过滤私聊/投递广播。
## 与网关服务器的数据流

ChatServer ↔ GatewayServer 使用 32 字节 `GateFrame`（魔数 `0xC1EA`，版本 1）。
头部含 `module/method/seq/playerId/totalLen/srcServiceID/dstServiceID`，服务号为
`ServerID::kChat=3`；请求与响应使用相同 `module=100/method=1` 和非零 `seq`，
推送使用 `seq=0`，头部 `playerId` 为目标玩家。字节偏移见
`GameServer/Zone/PROTOCOL.md`。

# 在线聊天服务

构建：`cmake -S Zone -B Zone/build && cmake --build Zone/build -j 2`

启动顺序：先启动网关（在 `gateway.privateConfig.allowedServices` 中加入 `3`、`4`，保持默认场景服务 `8`），再运行：

```
Zone/build/bin/chatServer 127.0.0.1 13145 1
Zone/build/bin/globalServer 127.0.0.1 13145 1
```

聊天服可以按相同格式重复追加同一区服的网关（每组三参数分别为网关**内网** IPv4 地址、端口和区服 ID），例如 `chatServer 127.0.0.1 13145 1 127.0.0.1 13146 1`；全局服仍只连接一个网关。所有网关必须允许服务号 3，并且客户端登录时能在本网关访问已注册的聊天服。重复地址/端口或不同区服 ID 会拒绝启动。任何一个网关链路断开，聊天进程会退出，须由部署系统重启；没有自动重连或离线消息。广播向所有已配置网关扇出，连接未配置的网关的客户端不会收到。

客户端鉴权后发送 ClientFrame **v2**（32 字节头，版本 2，角色名字段 12 字节），Request, module=100, method=1, requestId≠0；body 是
`目标 playerId(u64，大端) + UTF-8 消息(1..1024 字节)`，文本不允许控制字符；头部角色名为非空 UTF-8（≤12 字节，右侧零填充）。网关从会话获取真实发送人 ID，
将名字插入服务间请求体：`目标 playerId(u64 大端) + 12B角色名 + 消息`。
聊天服验证内容并发送两个帧：给发送人 Response（同 module/method/seq，网关恢复客户端 requestId，
body=`accepted` 或错误描述）；给目标用户 Push（网关转换为 requestId=0，body 为
`发送人 playerId(u64，大端) + 原消息`；推送的 ClientFrame 头部角色名为发送人提交的名字）。聊天服发往网关的推送体则是 `发送人 ID + 12B角色名 + 原消息`。
缺少目标 ID 的请求体由网关直接回 `kError`；目标 ID 为零、给自己私聊、文本为空/超长/编码非法等由聊天服返回文本响应（保留原 `module/method/seq`）。未知方法返回 `unsupported method`；不会生成推送。
`accepted` 仅表示响应和所有网关推送已进入聊天服出站队列，不表示接收者在线、已收到或已读。跨网关私聊由各网关筛选目标本地会话，无目标在线时推送被丢弃。

## 普通玩家全区喊话（联调）

鉴权客户端发送 `module=100,method=2`，body 为 UTF-8 文本（1..1024 字节、不允许控制字符），头部必须包含非空展示名。网关从会话注入可信发送方 ID，将 12B 展示名放在服务间请求体开头。聊天服校验后回复同请求号 `accepted`，向所有已配置网关发 `seq=0,playerId=0` 的广播推送，体为 `senderId(u64 BE)+name(12B)+text`。网关只对本地已鉴权会话发 `Push(module=100,method=2,requestId=0)`，ClientFrame 头部放展示名，body 为 `senderId(u64 BE)+text`；发送者本人也接收广播。无效文本回复 `invalid chat message`，不推送。

`module=100,method=3` 为预留的管理员广播方法，当前统一回复 `admin broadcast unavailable`，**不会广播**。开发期共享票据不是管理员身份凭证，不能通过客户端角色名、玩家 ID 或消息体字段伪造权限。
**名字未经账号/角色服务验证，可被发送人伪造，只能用于展示，不得用于鉴权或权限判断**。

测试：`ctest --test-dir Zone/build -R gateway_chat_global --output-on-failure`。