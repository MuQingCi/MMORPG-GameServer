# 最小全局服务

## 目录

- [本服务器简介](#本服务器简介)
- [构建与运行](#构建与运行)
- [Ping/Echo 与协议](#pingecho-与协议)
- [职责边界](#职责边界)
- [测试与适用范围](#测试与适用范围)

## 本服务器简介

`globalServer` 目前只提供 Ping/Echo，用于检查全局服务能否接入网关，以及响应能否返回原请求。它与聊天服共用网络/逻辑两线程运行时，主动连接单个网关，以 `serviceId=4` 注册；没有自动重连。

## 构建与运行

依赖与公共构建设置见[主 README](../../README.md#各子服务器的构建命令)。从仓库根目录执行：

```bash
cmake -S Zone -B out/zone -DSCENE_BUILD_TESTS=ON
cmake --build out/zone --target gatewayServer globalServer -j 2
```

在网关 `gateway.privateConfig.allowedServices` 中保留 `8` 并加入 `4`，再按[网关说明](../gatewayServer/README.md#构建与运行)启动网关。随后执行：

```bash
out/zone/bin/globalServer 127.0.0.1 13145 1
```

三个参数依次为网关内网 IPv4 地址、端口和区服 ID。Global 只连接一个网关，不提供自动重连。

## Ping/Echo 与协议

鉴权客户端发送 `module=101, method=1` 的 Request，服务原样返回 body，并保留 `module/method/seq/playerId`。body 可以是任意字节，不按文本或 UTF-8 解析，也不使用角色名字段。不支持的方法返回 `unsupported method`。

| 链路 | 协议 |
| --- | --- |
| 客户端 ↔ 网关 | **ClientFrame v2**：32 字节头、版本 2、12 字节角色名 |
| 网关 ↔ Global | **GateFrame v1**：32 字节头、版本 1、魔数 `0xC1EA` |

服务间用 `seq` 关联请求和响应，网关据此恢复客户端请求号，没有全局服务专用帧头。字节布局见[线协议](../PROTOCOL.md)。

## 职责边界

Global 目前只是区服内的接入示例，尚无全区状态、数据库、公会、排行榜、跨区路由或角色迁移功能，也不是跨 Zone 中心。LoginServer 尚未实现，规划负责账号认证、登录凭证、区服列表和角色归属查询。

## 测试与适用范围

Global 与 Chat 共用 `gateway_chat_global` 进程测试。构建并运行：

```bash
cmake --build out/zone --target gatewayServer chatServer globalServer -j 2
ctest --test-dir out/zone -R '^gateway_chat_global$' --output-on-failure
```

这里提供测试入口，不表示本次文档整理已执行或通过该用例。目前没有 Global 的独立吞吐和长时间稳定性测量。[主 README](../../README.md)中的 3G2S 数据来自移动/AOI 请求，不能当作 Global 的容量数据。
