# 最小全局服务

`globalServer` 使用与聊天服相同的网络/逻辑两线程运行时，反向连接单个网关并以
`serviceId=4` 握手。鉴权客户端发送 `module=101, method=1` 的 Request，
服务端按相同 module/method/seq/playerId 回传原 body，用于验证全局服务接入和
网关响应关联。暂不承担公会、全区状态或数据库等业务；未实现自动重连。

客户端使用 **ClientFrame v2**（32 字节头、版本 2、12 字节角色名），网关与全局服
之间仍用 **GateFrame v1**（32 字节头、版本 1、魔数 `0xC1EA`）；请求/响应由 `seq`
关联，不另定义全局服务专用帧头。全局 Ping/Echo 不使用角色名字段，body 原样回传。
body 可以是任意字节（不按文本或 UTF-8 解析）；不支持的方法返回 `unsupported method`，
响应头保留请求的 `module/method/seq/playerId`，以便网关恢复客户端请求号。
详细字节布局见 `GameServer/Zone/PROTOCOL.md`。

运行命令与限制参见 `GameServer/Zone/chatServer/README.md`。