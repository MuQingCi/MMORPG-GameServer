#ifndef CLEARMOON_COMMON_PROTO_H
#define CLEARMOON_COMMON_PROTO_H

#include "base/buffer.h"

#include <cstdint>
#include <string>

// 公共协议字段的语义别名。宽度以实际线格式为准：GateFrame/ClientFrame 的
// module、method 为 u16，totalLen 为 u32；握手/鉴权的 zoneId 为 u32。
// 这些都是 uintXX_t 的别名，不会提供不同于整数的类型安全性。
using MODULE = uint16_t;
using METHOD = uint16_t;
using LENGTH = uint32_t;
using ZoneID = uint32_t;
using InstanceID = uint32_t;
using SessionID = uint64_t;
using EpochID = uint32_t;

// 不提供旧 proto/proto.h 的 BackendService 和 MessageKind：
// BackendService::Scene=2 与当前 ServerID::kScene_1=8 冲突；
// MessageKind 的 Request/Response/Push 是 ClientKind（clientProto.h）语义，
// 不是 GateFrame::msgType（NetMessageType）。也不提供 VERSION 别名：
// ClientFrame 版本为 u8，而 GateFrame 版本为 u16。

/**
 * 场景服与网关/全局服/聊天服之间的线格式（固定 32 字节头）
 * -----------------------------------------------------------------------------------------------------
 * |2B魔数|2B版本|1B消息类型|1B重试标记|2B模块|2B方法|8B序列号|8B玩家ID|4B总长度|1B源服务|1B目的服务|业务数据|
 * -----------------------------------------------------------------------------------------------------
 * 偏移(十进制) :
   0      2     4          5         6      8     10      18       26       30       31        32
 *
 * 约定：
 *   - 全部多字节字段为**网络序（大端）**；
 *   - totalLen = 头(32) + 业务数据长度，即整帧长度；
 *   - 序列号/玩家ID/业务数据在解码后封装进 Msg。
 *
 * 为什么解码的过滤维度是 dstServiceID 而不是 msgType：
 *   msgType 表达的是"这条消息由谁产生/属于哪条链路"，同一条链路上既有业务消息
 *   也可能有控制消息，按类型一刀切会误丢；而"是不是发给我的"只能由 dstServiceID 表达。
 */

static constexpr uint16_t kMagic          = 0xC1EA;
static constexpr uint16_t kVersion        = 1;
static constexpr uint16_t kHeaderSize     = 32;
static constexpr uint32_t kMaxMessageSize = 10 * 1024 * 1024;  // 10MB

// 网络消息类型（链路标识，不是路由依据）
enum NetMessageType : uint8_t
{
    kGatewayMsg = 1,
    kDBMsg      = 2,
    kGlobalMsg  = 3,
    kChatMsg    = 4,
    kSceneMsg   = 10,
};

// 服务 id（srcServiceID / dstServiceID）
enum ServerID : uint8_t
{
    kServiceAny = 0,   // 仅用于"不过滤"（测试/工具）
    kGateway    = 1,
    kDB         = 2,
    kChat       = 3,
    kGlobal     = 4,
    kScene_1    = 8,
};

inline const char* ServerIdName(uint8_t id)
{
    switch (id)
    {
        case kServiceAny: return "ANY";
        case kGateway:    return "GATEWAY";
        case kDB:         return "DB";
        case kChat:       return "CHAT";
        case kGlobal:     return "GLOBAL";
        case kScene_1:    return "SCENE_1";
        default:          return "UNKNOWN";
    }
}

struct Header
{
    // 全部给默认值：Header 未初始化曾是\"随机 module/method\"这类诡异 bug 的来源
    uint8_t  retrFlag     = 0;
    uint8_t  msgType      = 0;   // NetMessageType
    uint16_t module       = 0;
    uint16_t method       = 0;
    uint64_t seq          = 0;
    uint64_t playerId     = 0;
    uint32_t totalLen     = 0;   // 编码时由 body 长度推出；解码时原样给出
    uint8_t  srcServiceID = 0;
    uint8_t  dstServiceID = 0;
};

// ---------------------------------------------------------------------------
// SYS 通道（module = 1）在**服务间链路**上的方法号。
//
// 为什么放在公共协议层：这条链路上唯一必须两侧一致的控制消息就是"后端服务向网关
// 上报身份"的握手。把常量留在各自服务里必然漂移（一侧改号、另一侧静默不解帧），
// 场景服的 Method::SYS_HANDSHAKE 现在直接引用这里的值。
// ---------------------------------------------------------------------------
namespace SysModule
{
constexpr uint16_t kSYS = 1;   // 与 sceneServer 的 Module::SYS 同值
}

namespace SysMethod
{
constexpr uint16_t kHandshake = 1;   // 后端 -> 网关：上报服务身份（握手体见下）
}

// 简化版服务业务通道：请求/响应使用相同的 module/method 和非零 seq，
// 私聊推送使用 seq=0，playerId=接收方。后端不可使用其它模块冒充聊天消息。
namespace ServiceModule
{
constexpr uint16_t kChat = 100;
constexpr uint16_t kGlobal = 101;
}
namespace ServiceMethod
{
constexpr uint16_t kPrivateChat = 1;
constexpr uint16_t kZoneShout = 2;
constexpr uint16_t kAdminBroadcast = 3; // reserved: requires trusted authorization
constexpr uint16_t kGlobalPing = 1;
}

/**
 * @brief 服务间握手体（后端服务 -> 网关）
 *
 * 布局：serviceId(u8) + zoneId(u32) + sceneId(u32) = 9 字节，整数为**小端**
 * （与 sceneServer 既有 HandshakeBody 的 PutU32 布局一致，因此线上格式未变）。
 * 握手体属于链路控制信息，不需要 protobuf，也**不允许客户端伪造**：
 * 客户端链路上根本没有这个 method（客户端协议是独立的 ClientFrame）。
 */
struct BackendHandshake
{
    uint8_t  serviceId = 0;
    uint32_t zoneId    = 0;
    uint32_t sceneId   = 0;
};

std::string PackBackendHandshake(const BackendHandshake& h);
bool        UnpackBackendHandshake(const std::string& body, BackendHandshake& out);

// 出站帧头组装（传输层字段由网络层负责填，业务层只填 Module/Method/playerId/seq）
Header MakeHeader(uint8_t msgType,
                  uint16_t module,
                  uint16_t method,
                  uint64_t seq,
                  uint64_t playerId,
                  uint8_t srcServiceID,
                  uint8_t dstServiceID);

/**
 * @brief 把一条完整消息编码追加到 out 尾部
 *
 * 注意：只往 out **追加**，绝不假设 out 是空的。
 * 旧实现直接对"连接的 sendBuffer"编码，会把上一轮未发完的残余
 * 与新消息当成一条消息算长度，帧边界永久错乱。
 */
void EncodeFrame(Buffer& out, const Header& head, const std::string& body);

/**
 * @brief 解码三态
 *   kOk       : 成功解出一帧（head/body 有效，buf 已消费该帧）
 *   kNeedMore : 半包，等下次可读事件（buf 未被消费）
 *   kError    : 协议错误（帧头字段非法、对端服务不匹配等），buf 已做重同步
 */
enum class DecodeStatus
{
    kOk,
    kNeedMore,
    kError,
};

/**
 * @brief 从 buf 中解出一帧
 *
 * @param expectedDstServiceID 本服务 id；帧头 dstServiceID 不匹配时视为"不是发给我的"
 *                             （传 kServiceAny 表示不过滤，仅测试使用）
 *
 * 语义保证：
 *   - 返回 kOk 时 body 恰好是这一帧的业务数据，buf 从下一帧开始；
 *   - 返回 kNeedMore 时不消费任何数据（含"没有魔数"的垃圾前缀清理例外：
 *     会丢弃到最多保留 1 字节，避免无界积压）；
 *   - 返回 kError 时已尽力重同步到下一个候选魔数之后，调用方应计数并在达到阈值时断开。
 */
DecodeStatus DecodeFrame(Buffer& buf,
                         Header& head,
                         uint8_t expectedDstServiceID,
                         std::string& body);

#endif
