# 最小全局服务

`globalServer` 使用与聊天服相同的网络/逻辑两线程运行时，反向连接单个网关并以
`serviceId=4` 握手。鉴权客户端发送 `module=101, method=1` 的 Request，
服务端按相同 module/method/seq/playerId 回传原 body，用于验证全局服务接入和
网关响应关联。暂不承担公会、全区状态或数据库等业务；未实现自动重连。

运行命令与限制参见 `/home/lanxiyuan/Project_Cpp/GameServer/Zone/chatServer/README.md`。