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