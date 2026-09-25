#include "base/clientProto.h"

#include "base/utils.h"
#include "log/logger.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace
{
// 单次解码调用内的最大重同步次数（与 GateFrame 一致，防止伪造魔数把 IO 线程拖死）
constexpr int kMaxResyncPerCall = 16;
// 没有魔数时保留的尾部字节数：魔数可能被拆包，留 1 字节等下次补齐
constexpr size_t kKeepTailWhenNoMagic = 1;

size_t FindClientMagic(Buffer& buf)
{
    const size_t readable = buf.readableBytes();
    if (readable < sizeof(uint16_t))
        return static_cast<size_t>(-1);

    // 魔数在大端字节序下是 (0xC1, 0xEB)
    const char magicBytes[2] = {
        static_cast<char>((kClientMagic >> 8) & 0xFF),
        static_cast<char>(kClientMagic & 0xFF),
    };

    const char* start = buf.peek();
    const char* end = start + readable;
    const char* pos = std::search(start, end, magicBytes, magicBytes + 2);
    if (pos == end)
        return static_cast<size_t>(-1);

    return static_cast<size_t>(pos - start);
}
}  // namespace

void EncodeClientFrame(Buffer& out, const ClientHeader& h, const std::string& body)
{
    const uint32_t totalLen = static_cast<uint32_t>(kClientHeaderSize + body.size());

    char hdr[kClientHeaderSize];
    const uint16_t magicNet  = host16ToNet(kClientMagic);
    const uint16_t moduleNet = host16ToNet(h.module);
    const uint16_t methodNet = host16ToNet(h.method);
    const uint64_t reqNet    = host64ToNet(h.requestId);
    const uint32_t lenNet    = host32ToNet(totalLen);

    std::memcpy(hdr + 0, &magicNet, sizeof(magicNet));    // +0  魔数
    hdr[2] = static_cast<char>(kClientVersion);           // +2  版本
    hdr[3] = static_cast<char>(h.kind);                   // +3  消息类型
    std::memcpy(hdr + 4, &moduleNet, sizeof(moduleNet));  // +4  模块
    std::memcpy(hdr + 6, &methodNet, sizeof(methodNet));  // +6  方法
    std::memcpy(hdr + 8, &reqNet, sizeof(reqNet));        // +8  请求号
    std::memcpy(hdr + 16, &lenNet, sizeof(lenNet));       // +16 整帧长度

    out.append(hdr, sizeof(hdr));
    if (!body.empty())
        out.append(body.data(), body.size());
}

DecodeStatus DecodeClientFrame(Buffer& buf, ClientHeader& h, std::string& body)
{
    int resync = 0;

    for (;;)
    {
        if (buf.readableBytes() < kClientHeaderSize)
            return DecodeStatus::kNeedMore;

        const size_t offset = FindClientMagic(buf);
        if (offset == static_cast<size_t>(-1))
        {
            // 没有魔数：全是脏数据，只留可能的半截魔数
            const size_t readable = buf.readableBytes();
            const size_t drop = readable > kKeepTailWhenNoMagic ? readable - kKeepTailWhenNoMagic : 0;
            if (drop > 0)
            {
                LOG_DEBUG << "clientProto: drop " << drop << " bytes without magic";
                buf.retrieve(drop);
            }
            return DecodeStatus::kNeedMore;
        }

        if (offset > 0)
        {
            LOG_DEBUG << "clientProto: resync, drop " << offset << " bytes";
            buf.retrieve(offset);
            if (++resync > kMaxResyncPerCall)
            {
                LOG_WARNING << "clientProto: too many resync in one call";
                return DecodeStatus::kError;
            }
            if (buf.readableBytes() < kClientHeaderSize)
                return DecodeStatus::kNeedMore;
        }

        // 版本校验失败时跳过"当前这个候选魔数"，避免 offset==0 时死循环
        if (static_cast<uint8_t>(buf.peek()[2]) != kClientVersion)
        {
            LOG_DEBUG << "clientProto: bad version, skip one candidate";
            buf.retrieve(sizeof(uint16_t));
            if (++resync > kMaxResyncPerCall)
                return DecodeStatus::kError;
            continue;
        }

        uint32_t totalLenNet = 0;
        std::memcpy(&totalLenNet, buf.peek() + 16, sizeof(totalLenNet));
        const uint32_t totalLen = netToHost32(totalLenNet);

        if (totalLen < kClientHeaderSize || totalLen > kMaxClientFrameSize)
        {
            LOG_DEBUG << "clientProto: illegal totalLen=" << totalLen << ", skip one candidate";
            buf.retrieve(sizeof(uint16_t));
            if (++resync > kMaxResyncPerCall)
                return DecodeStatus::kError;
            continue;
        }

        if (totalLen > buf.readableBytes())
            return DecodeStatus::kNeedMore;   // 半包

        // 消费整帧头（逐字段 pop，顺序与编码严格一致）
        buf.readUint16();                                  // magic（已校验）
        h.totalLen = totalLen;
        buf.readUint8();                                   // version（已校验）
        h.kind      = static_cast<ClientKind>(buf.readUint8());
        h.module    = buf.readUint16();
        h.method    = buf.readUint16();
        h.requestId = buf.readUint64();
        buf.readUint32();                                  // totalLen（已校验）

        const uint32_t bodyLen = totalLen - kClientHeaderSize;
        if (buf.readableBytes() < bodyLen)
            return DecodeStatus::kNeedMore;

        body = buf.readAsString(bodyLen);
        return DecodeStatus::kOk;
    }
}

// ============ 定长控制体（小端，与场景服 HandshakeBody 的既有布局一致）============
void ClientPutU32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void ClientPutU64(std::string& s, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

bool ClientGetU32(const std::string& s, size_t off, uint32_t& v)
{
    if (s.size() < off + 4)
        return false;
    v = uint32_t(uint8_t(s[off])) | (uint32_t(uint8_t(s[off + 1])) << 8) |
        (uint32_t(uint8_t(s[off + 2])) << 16) | (uint32_t(uint8_t(s[off + 3])) << 24);
    return true;
}

bool ClientGetU64(const std::string& s, size_t off, uint64_t& v)
{
    if (s.size() < off + 8)
        return false;
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i)
        out |= uint64_t(uint8_t(s[off + i])) << (8 * i);
    v = out;
    return true;
}

std::string MakeAuthBody(uint64_t playerId, uint64_t ticket)
{
    std::string s;
    ClientPutU64(s, playerId);
    ClientPutU64(s, ticket);
    return s;
}

bool ParseAuthBody(const std::string& body, uint64_t& playerId, uint64_t& ticket)
{
    return ClientGetU64(body, 0, playerId) && ClientGetU64(body, 8, ticket);
}

std::string MakeAuthAckBody(uint64_t playerId, uint64_t sessionId, uint32_t epoch,
                            uint32_t zoneId, uint8_t dstService)
{
    std::string s;
    ClientPutU64(s, playerId);
    ClientPutU64(s, sessionId);
    ClientPutU32(s, epoch);
    ClientPutU32(s, zoneId);
    s.push_back(static_cast<char>(dstService));
    return s;
}

bool ParseAuthAckBody(const std::string& body, uint64_t& playerId, uint64_t& sessionId,
                      uint32_t& epoch, uint32_t& zoneId, uint8_t& dstService)
{
    if (body.size() < 25)
        return false;
    // 每个字段都有边界检查（ClientGet* 内部判长度），短包一律拒绝而不是读越界
    if (!ClientGetU64(body, 0, playerId) || !ClientGetU64(body, 8, sessionId) ||
        !ClientGetU32(body, 16, epoch) || !ClientGetU32(body, 20, zoneId))
        return false;
    dstService = static_cast<uint8_t>(body[24]);
    return true;
}

std::string MakeServiceBody(uint8_t serviceId)
{
    return std::string(1, static_cast<char>(serviceId));
}

bool ParseServiceBody(const std::string& body, uint8_t& serviceId)
{
    if (body.empty())
        return false;
    serviceId = static_cast<uint8_t>(body[0]);
    return true;
}
