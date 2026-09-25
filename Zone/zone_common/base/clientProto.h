#ifndef CLEARMOON_COMMON_CLIENTPROTO_H
#define CLEARMOON_COMMON_CLIENTPROTO_H

#include "base/buffer.h"
#include "base/proto.h"   // DecodeStatus（两种编解码共用三态）

#include <cstdint>
#include <string>

/**
 * 客户端接入帧（ClientFrame）——**公网客户端 <-> 网关**的线格式
 * ============================================================================
 *  与 base/proto.h 的 32 字节服务间帧（GateFrame）是**两套独立协议**，
 *  不做字段复用、不共享 header：
 *    - 服务间帧面向"服务到服务"，需要 src/dst 服务号、链路类型、playerId 等服务内部信息；
 *    - 客户端帧面向不可信公网，**不允许客户端携带可信身份**（playerId/zoneId 由网关绑定），
 *      但必须带 `kind` 与 `requestId` 以便网关做请求/响应配对。
 *
 * ---------------------------------------------------------------------------
 * |2B魔数|1B版本|1B类型|2B模块|2B方法|8B请求号|4B总长度|业务数据|
 * ---------------------------------------------------------------------------
 *  偏移(十进制):  0      2       3      4      6      8        16       20
 *
 * 约定：
 *   - 头部全部多字节字段为**网络序（大端）**，与 GateFrame 保持一致；
 *   - totalLen = 20(头) + body 长度，即整帧长度；
 *   - requestId 由客户端生成、响应原样回带（作用域 = 该客户端的会话内）；
 *   - 定长控制体（Auth/AuthAck/...）内的整数用**小端**，与场景服 HandshakeBody 的
 *     既有布局（`PutU32`）保持一致，避免同一进程内出现两种定长体风格。
 *
 * 与 GateFrame 的区别要点（"区分二者"的第一层）：
 *   - 魔数不同（0xC1EB vs 0xC1EA）→ 同一端口上误接另一套协议会被立刻识别为脏数据；
 *   - 帧头长度不同（20 vs 32）与字段不同 → 客户端无法伪造服务间字段。
 */
static constexpr uint16_t kClientMagic      = 0xC1EB;
static constexpr uint8_t  kClientVersion    = 1;
static constexpr uint16_t kClientHeaderSize = 20;
// 客户端单帧上限：公网入口必须比服务间上限更紧（GateFrame 是 10MB）
static constexpr uint32_t kMaxClientFrameSize = 64 * 1024;

// 消息类型（对应设计文档的 MessageKind）
enum class ClientKind : uint8_t
{
    kRequest  = 1,   // 客户端请求（网关转发给后端服务，需已鉴权）
    kResponse = 2,   // 网关回给客户端的正常响应
    kPush     = 3,   // 网关主动推送（后端服务发起，无对应请求）
    kError    = 4,   // 明确的错误响应（鉴权失败/无可用后端/未鉴权访问业务）
    kControl  = 5,   // 控制消息（鉴权、绑定后端服务、心跳）
};

// 控制通道的模块号：与 GateFrame 的 Module::SYS(1) 同值，便于两侧日志对照
static constexpr uint16_t kClientSysModule = 1;

// 控制通道的方法号（客户端 <-> 网关，只在本段链路上有效）
namespace ClientSysMethod
{
constexpr uint16_t kAuth           = 1;   // C->G: body = playerId(u64) + ticket(u64)
constexpr uint16_t kAuthAck        = 2;   // G->C: body = playerId + sessionId + epoch + zoneId + dstService
constexpr uint16_t kAuthFail       = 3;   // G->C: body = 文本
constexpr uint16_t kBindService    = 4;   // C->G: body = serviceId(u8)
constexpr uint16_t kBindServiceAck = 5;   // G->C: body = serviceId(u8)
constexpr uint16_t kPing           = 6;   // C->G
constexpr uint16_t kPong           = 7;   // G->C
constexpr uint16_t kReject         = 8;   // G->C: body = 文本（未鉴权/无可用后端/协议越权）
}  // namespace ClientSysMethod

struct ClientHeader
{
    ClientKind kind      = ClientKind::kRequest;
    uint16_t   module    = 0;
    uint16_t   method    = 0;
    uint64_t   requestId = 0;
    uint32_t   totalLen  = 0;   // 编码时由 body 长度推出；解码时原样给出
};

/**
 * @brief 把一条完整的客户端帧追加到 out 尾部（语义与 EncodeFrame 一致：只追加）
 */
void EncodeClientFrame(Buffer& out, const ClientHeader& h, const std::string& body);

/**
 * @brief 从 buf 中解出一帧客户端帧
 *
 * 三态语义与 DecodeFrame 完全一致（kOk 消费一帧 / kNeedMore 不消费 / kError 已尽力重同步）。
 * 不接收 expectedDstServiceID：客户端帧里没有服务号，路由目标由**网关的会话状态**决定
 * （客户端不能通过包头指定任意后端服务）。
 */
DecodeStatus DecodeClientFrame(Buffer& buf, ClientHeader& h, std::string& body);

// ============ 定长控制体（小端）============
void ClientPutU32(std::string& s, uint32_t v);
void ClientPutU64(std::string& s, uint64_t v);
bool ClientGetU32(const std::string& s, size_t off, uint32_t& v);
bool ClientGetU64(const std::string& s, size_t off, uint64_t& v);

// Auth: playerId + ticket（16B）
std::string MakeAuthBody(uint64_t playerId, uint64_t ticket);
bool        ParseAuthBody(const std::string& body, uint64_t& playerId, uint64_t& ticket);

// AuthAck: playerId + sessionId + epoch + zoneId + dstService（25B）
std::string MakeAuthAckBody(uint64_t playerId, uint64_t sessionId, uint32_t epoch,
                            uint32_t zoneId, uint8_t dstService);
bool        ParseAuthAckBody(const std::string& body, uint64_t& playerId, uint64_t& sessionId,
                             uint32_t& epoch, uint32_t& zoneId, uint8_t& dstService);

// BindService / BindServiceAck: serviceId（1B）
std::string MakeServiceBody(uint8_t serviceId);
bool        ParseServiceBody(const std::string& body, uint8_t& serviceId);

#endif
