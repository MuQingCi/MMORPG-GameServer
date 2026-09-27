# 聊天服务器
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
- 聊天服务器不操作玩家数据、只负责根据src/dst PlayerId、playerName、body组装聊天帧ChatMsg并将其回投对应网关
- 内部只持有网关会话表gatewaySessionTable和玩家Id所处网关速查表pGatewayTable  


> gatewaySessionTable——gatewayId --> set/vector<playerEntry>
> pGatewayTable——  playerId->gatewayId
```c++
struct playerEntry
{
    uint64_t playerId;
    std::string playerName;

}
```
## 与网关服务器的数据流

ChatServer <————ChatMsg————> GatewayServer
> ChatMsg格式如下
> [2B魔数|2B版本|2B模块|2B方法|8B源玩家ID|8B目的玩家ID|业务数据body]——暂定头部为24Bytes

# 最小单网关私聊服务

构建：`cmake -S Zone -B Zone/build && cmake --build Zone/build -j 2`

启动顺序：先启动网关（在 `gateway.privateConfig.allowedServices` 中加入 `3`、`4`，保持默认场景服务 `8`），再运行：

```
Zone/build/bin/chatServer 127.0.0.1 13145 1
Zone/build/bin/globalServer 127.0.0.1 13145 1
```

三个参数分别为网关**内网** IPv4 地址、端口和区服 ID；区服 ID 必须和网关一致。
本实现只主动连接**一个**网关；没有自动重连、跨网关私聊、可靠离线消息。

客户端鉴权后发送 ClientFrame(Request, module=100, method=1, requestId≠0)，body 是
`目标 playerId(u64，大端) + UTF-8 消息(1..1024 字节)`。网关从会话获取真实发送人身份，
聊天服验证内容并发送两个帧：给发送人 Response（同 module/method，requestId 原值，
body=`accepted` 或错误描述）；给目标用户 Push（requestId=0，body 为
`发送人 playerId(u64，大端) + 原消息`）。`accepted` 仅表示已进入聊天服出站队列，
不表示接收者在线、已收到或已读。玩家昵称尚无可信来源，本阶段只传 playerId。

测试：`ctest --test-dir Zone/build -R gateway_chat_global --output-on-failure`。