# Gateway 链路协议（客户端 v2 / 服务间 v1）

本文件约定 **Client ↔ Gateway** 与 **backendService ↔ Gateway** 两条独立的 TCP 字节流协议；实现分别位于 `zone_common/base/clientProto.{h,cc}` 与 `zone_common/base/proto.{h,cc}`。客户端 v2 新增角色名；旧 v1 客户端不兼容；服务间 v1 的头和场景服保持不变。

## 通用规则

- 每个 TCP 连接只使用所属监听端口的一种协议；帧可拆包/粘包；`totalLen` **包含帧头**。整数帧头字段均为网络字节序（大端），定长控制体另按各节规定。body 一般原样透传（如场景服 protobuf）；聊天私聊在网关处增加/提取角色名，详见下文。
- 客户端版本固定为 2，服务间版本固定为 1，不做隐式降级；魔数、版本或长度错误不得进入业务层。客户端帧最长 **65536 B（含头）**，服务间帧最长 **10485760 B（含头）**。编码方必须在调用现有 `void Encode*` 前保证大小不超过对应上限；目前编码器本身**没有返回错误/上限检查**，因此出站超长帧仍是后续待补的边界。
- 不同魔数标识不同协议，**不代表身份验证**。网关先用端口确定链路角色，再检验魔数、握手与状态；内网仅靠 IP/白名单/明文握手不能防止能够访问内网端口的攻击者冒充后端。

## 客户端链路（ClientFrame）

公网端口（`publicConfig`）；32 B 固定头；魔数 `C1 EB`、版本 `02`。

| 偏移 | 长度 | 字段 | 约定 |
|---:|---:|---|---|
| 0 | 2 | magic | `0xC1EB` |
| 2 | 1 | version | `2` |
| 3 | 1 | kind | Request=1, Response=2, Push=3, Error=4, Control=5 |
| 4 | 2 | module | `1` 留给 SYS 控制通道 |
| 6 | 2 | method | 模块内方法号 |
| 8 | 8 | requestId | 客户端会话内的关联号；业务 Request 必须非零；Response/Error 回显；Push 为零 |
| 16 | 4 | totalLen | `32 + body.size()` |
| 20 | 12 | roleName | UTF-8 字节（最多 12 B），不足右侧零填充；12 B 恰好填满时无需终止符 |
| 32 | 可变 | body | SYS 为下述定长体/错误文本；业务体由应用层定义 |

客户端只可发送 `(Control, SYS=1)` 或 `(Request, 非 SYS)`；Response/Push/Error 仅网关出站，`kind` 与 `module` 不匹配的控制请求被拒。客户端帧**没有可信 playerId、zoneId、src/dst**；网关从已鉴权的会话取 playerId，并从会话绑定的服务取目的服务。`BindService` 仅能选择网关白名单中的服务，不能任意指定目的地址。角色名由客户端提供，仅校验 UTF-8、长度和零填充；不能作为可信昵称或权限依据。聊天请求要求非空角色名。

控制通道 `module=1`（requestId 在响应中回显）：

| method | 方向 | body |
|---:|---|---|
| 1 Auth | C→G | `playerId(u64 LE) + ticket(u64 LE)`，严格 16 B |
| 2 AuthAck | G→C | `playerId(u64 LE) + sessionId(u64 LE) + epoch(u32 LE) + zoneId(u32 LE) + dstService(u8)`，严格 25 B |
| 3 AuthFail | G→C（Error） | 错误文本 |
| 4 BindService | C→G | `serviceId(u8)`，严格 1 B |
| 5 BindServiceAck | G→C | `serviceId(u8)`，严格 1 B |
| 6 Ping / 7 Pong | C→G / G→C | 当前发送端 Pong 为空；Ping 的 body 尚未严格校验 |
| 8 Reject | G→C | 预留错误方法号；当前通用错误实际使用 `kind=Error, module=SYS, method=原请求 method`，body 为文本 |

**鉴权边界**：当前 ticket 是所有客户端共享的配置数值，不是登录服务签发的按用户绑定凭证；`clientToken=0` 会禁用票据验证，只能用于本地联调。生产环境需要另行设计登录凭证、传输加密和重放防护，不能把当前机制称为安全认证。

## 服务间链路（GateFrame）

内网端口（`privateConfig`）；32 B 固定头；魔数 `C1 EA`、版本 `00 01`。

| 偏移 | 长度 | 字段 | 约定 |
|---:|---:|---|---|
| 0 | 2 | magic | `0xC1EA` |
| 2 | 2 | version | `1` |
| 4 | 1 | msgType | 既有链路标识（Gateway=1, DB=2, Global=3, Chat=4, Scene=10），**不是** Request/Response/Push 类别 |
| 5 | 1 | retrFlag | 网关业务链路当前只接受 `0`；不启用应用层自动重传 |
| 6 | 2 | module | `1` 留给 SYS 握手 |
| 8 | 2 | method | 模块内方法号 |
| 10 | 8 | seq | 网关转发时分配的非零内部关联号；响应原样回传；主动推送为零 |
| 18 | 8 | playerId | 网关请求从会话绑定身份获得；回包应原样携带；推送用目标玩家 ID |
| 26 | 4 | totalLen | `32 + body.size()` |
| 30 | 1 | srcServiceID | 后端出站必须与已注册 serviceId 相同 |
| 31 | 1 | dstServiceID | 入站网关为 `kGateway=1`；后端按本服务 ID 检查 |
| 32 | 可变 | body | 握手体见下；业务体按 module/method 解释 |

后端连接后先发 `(module=1, method=1)` 握手，body 为严格 9 B：`serviceId(u8) + zoneId(u32 LE) + sceneId(u32 LE)`；头部 `srcServiceID == body.serviceId`，`seq=playerId=retrFlag=0`，目标为网关。网关按允许的 serviceId 和 zoneId 注册；注册前业务帧断连。同号重连取后到者，旧连接关闭。`sceneId` 当前只记录、不用于多实例路由；握手成功**没有协议层 Ack**，也没有心跳或主动健康探测。

网关转发客户端 Request：`src=kGateway, dst=会话目标服务, seq=网关唯一内部序号, playerId=会话绑定值`；响应须从对应服务连接发回，且 `src/playerId/module/method` 与原请求一致。网关消费回程路由后恢复客户端 `requestId` 并发送 Response。只有 **`seq=0 && playerId!=0`** 是推送；非零但未知的 `seq`（迟到、重复或已过期响应）丢弃，不能冒充 Push。路由有容量/TTL 上限，会话关闭与顶号时清理旧路由。

## 聊天服与全局服业务帧

- 聊天服 `serviceId=3, module=100, method=1`：Client 请求体 `targetPlayerId(u64 BE)+text(1..1024 B)`；网关插入头部角色名后，聊天服收到 `targetPlayerId(u64 BE)+name(12B)+text`；推送体为 `senderPlayerId(u64 BE)+name(12B)+text`，网关将名字放入 ClientFrame 头，客户端收到 `senderPlayerId(u64 BE)+text`。聊天 Response 的同 module/method/seq 用于关联请求。名字来自发送者客户端，**可伪造**。
- 聊天服 `module=100,method=2`：普通玩家全区喊话。Client 请求体 `text(1..1024 B)`，网关注入 `name(12B)+text`；聊天服向所有已配置网关推送 `seq=0,playerId=0,body=senderId(u64 BE)+name(12B)+text`，各网关向本地已鉴权客户端发 `Push(module=100,method=2,requestId=0,body=senderId(u64 BE)+text)`。`method=1` 跨网关私聊也向所有网关扇出，由本地玩家索引筛选目标；无可靠离线投递。`method=3` 预留管理员广播，在可信授权机制上线前只回复 `admin broadcast unavailable`，不推送。
- 全局服 `serviceId=4, module=101, method=1`：最小 Ping/Echo，直接回传 body 并沿用 seq/playerId/module/method；不使用角色名，不支持广播。两种服务都继续用 32B GateFrame，不定义另一套不兼容的帧头。

## 升级边界与待办

- 此 v1 帧不含 traceId、deadline、幂等键或显式服务间 MessageKind；如要支持响应方法号不同于请求、服务间错误类别、超时预算及多实例场景路由，先设计 **v2** 并补跨版本测试，不能在 v1 悄悄改变现有字节偏移或含义。
- `proto/proto.h` 现仅为旧路径兼容头，统一转发到 `zone_common/base/proto.h`；旧草案的 `BackendService` / `MessageKind` 已移除。服务号以 `ServerID` 为准（场景服 `kScene_1=8`），客户端消息类别以 `clientProto.h` 的 `ClientKind` 为准，不能把类别当成服务间帧的 `msgType`。
- 目前网关按连接上的注册身份校验来源，但单纯的明文握手无法提供密码学认证；仍需登录票据升级、后端 mTLS/可信网络、握手超时和出站长度检查。