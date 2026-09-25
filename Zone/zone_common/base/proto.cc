#include "base/proto.h"

#include "base/utils.h"
#include "log/logger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
// 单次解码调用内的最大重同步次数：防止"满屏伪魔数"把网络线程拖死
constexpr int kMaxResyncPerCall = 16;

// 没有魔数时保留的尾部字节数：魔数可能被拆包，留 1 字节等下次补齐
constexpr size_t kKeepTailWhenNoMagic = 1;

/**
 * @brief 在当前可读区域内寻找魔数（网络序两字节）
 * @return 相对 peek() 的偏移；未找到返回 npos
 */
size_t FindMagic(Buffer& buf)
{
    const size_t readable = buf.readableBytes();
    if (readable < sizeof(uint16_t))
        return static_cast<size_t>(-1);

    // 魔数在帧里是**大端字节序**：先高字节后低字节（0xC1, 0xEA）。
    // 这里绝不能再套一次 host16ToNet：那会得到 (0xEA, 0xC1)，
    // 与帧里的字节序相反 -> 永远搜不到魔数，重同步逻辑直接失效。
    const char magicBytes[2] = {
        static_cast<char>((kMagic >> 8) & 0xFF),
        static_cast<char>(kMagic & 0xFF),
    };

    const char* start = buf.peek();
    const char* end = start + readable;
    const char* pos = std::search(start, end, magicBytes, magicBytes + 2);
    if (pos == end)
        return static_cast<size_t>(-1);

    return static_cast<size_t>(pos - start);
}

// 跳过"当前这个候选魔数"，让下次搜索从它之后开始。
// 旧实现用 retrieve(offset)：魔数恰好位于偏移 0 时 retrieve(0) 一个字节都没消费，
// 一旦 msgType/dst 不匹配就会永久死循环并把网络线程打满 CPU。
void SkipCurrentMagic(Buffer& buf)
{
    buf.retrieve(sizeof(uint16_t));
}
}  // namespace

Header MakeHeader(uint8_t msgType,
                  uint16_t module,
                  uint16_t method,
                  uint64_t seq,
                  uint64_t playerId,
                  uint8_t srcServiceID,
                  uint8_t dstServiceID)
{
    Header h;
    h.retrFlag = 0;
    h.msgType = msgType;
    h.module = module;
    h.method = method;
    h.seq = seq;
    h.playerId = playerId;
    h.srcServiceID = srcServiceID;
    h.dstServiceID = dstServiceID;
    return h;
}

// 握手体：u8 + u32(little) + u32(little) = 9 字节。
// 这里**不**用 Buffer::readUint32（它是网络序），必须与小端布局严格一致。
std::string PackBackendHandshake(const BackendHandshake& h)
{
    std::string s;
    s.reserve(9);
    s.push_back(static_cast<char>(h.serviceId));
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((h.zoneId >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((h.sceneId >> (8 * i)) & 0xFF));
    return s;
}

bool UnpackBackendHandshake(const std::string& body, BackendHandshake& out)
{
    if (body.size() < 9)
        return false;

    out.serviceId = static_cast<uint8_t>(body[0]);
    uint32_t zoneId = 0;
    uint32_t sceneId = 0;
    for (int i = 0; i < 4; ++i)
    {
        zoneId |= uint32_t(static_cast<uint8_t>(body[1 + i])) << (8 * i);
        sceneId |= uint32_t(static_cast<uint8_t>(body[5 + i])) << (8 * i);
    }
    out.zoneId = zoneId;
    out.sceneId = sceneId;
    return true;
}

void EncodeFrame(Buffer& out, const Header& head, const std::string& body)
{
    const uint32_t totalLen = static_cast<uint32_t>(kHeaderSize + body.size());

    // 先把 32 字节头组装成一段连续内存，再整体 append。
    //
    // 为什么不用 Buffer::prepend：
    //   prepend 是"往可读区之前写"。当 out 里**已经有数据**（粘包场景：同一批里编第二帧）
    //   时，新帧头会被插到旧数据前面，缓冲里的帧顺序直接错乱。
    //   按偏移顺序 append 与 out 现有内容完全无关，语义稳定。
    char hdr[kHeaderSize];

    const uint16_t magicNet   = host16ToNet(kMagic);
    const uint16_t versionNet = host16ToNet(kVersion);
    const uint16_t moduleNet  = host16ToNet(head.module);
    const uint16_t methodNet  = host16ToNet(head.method);
    const uint64_t seqNet     = host64ToNet(head.seq);
    const uint64_t playerNet  = host64ToNet(head.playerId);
    const uint32_t lenNet     = host32ToNet(totalLen);

    // 每一步都写入"网络序值的内存表示"，即大端字节序列（与主机字节序无关）
    std::memcpy(hdr + 0,  &magicNet,   sizeof(magicNet));    // +0  魔数
    std::memcpy(hdr + 2,  &versionNet, sizeof(versionNet));  // +2  版本
    hdr[4] = static_cast<char>(head.msgType);                             // +4  网络消息类型
    hdr[5] = static_cast<char>(head.retrFlag);                            // +5  重试标记
    std::memcpy(hdr + 6,  &moduleNet,  sizeof(moduleNet));   // +6  模块
    std::memcpy(hdr + 8,  &methodNet,  sizeof(methodNet));   // +8  方法
    std::memcpy(hdr + 10, &seqNet,     sizeof(seqNet));      // +10 序列号
    std::memcpy(hdr + 18, &playerNet,  sizeof(playerNet));   // +18 玩家ID
    std::memcpy(hdr + 26, &lenNet,     sizeof(lenNet));      // +26 整帧长度
    hdr[30] = static_cast<char>(head.srcServiceID);                       // +30 源服务
    hdr[31] = static_cast<char>(head.dstServiceID);                       // +31 目的服务

    out.append(hdr, sizeof(hdr));
    if (!body.empty())
        out.append(body.data(), body.size());
}

DecodeStatus DecodeFrame(Buffer& buf,
                         Header& head,
                         uint8_t expectedDstServiceID,
                         std::string& body)
{
    int resync = 0;

    for (;;)
    {
        if (buf.readableBytes() < kHeaderSize)
            return DecodeStatus::kNeedMore;

        // 1. 定位魔数（快速路径：就在当前位置）
        const size_t offset = FindMagic(buf);
        if (offset == static_cast<size_t>(-1))
        {
            // 全是垃圾：丢掉绝大部分，只留可能的半截魔数
            const size_t readable = buf.readableBytes();
            const size_t drop = readable > kKeepTailWhenNoMagic ? readable - kKeepTailWhenNoMagic : 0;
            if (drop > 0)
            {
                LOG_DEBUG << "proto: drop " << drop << " bytes without magic";
                buf.retrieve(drop);
            }
            return DecodeStatus::kNeedMore;
        }

        if (offset > 0)
        {
            LOG_DEBUG << "proto: resync, drop " << offset << " bytes";
            buf.retrieve(offset);
            if (++resync > kMaxResyncPerCall)
            {
                LOG_WARNING << "proto: too many resync in one call";
                return DecodeStatus::kError;
            }
            if (buf.readableBytes() < kHeaderSize)
                return DecodeStatus::kNeedMore;
        }

        // 此处 buf[0..1] 已是魔数
        uint16_t versionNet = 0;
        std::memcpy(&versionNet, buf.peek() + 2, sizeof(versionNet));
        if (netToHost16(versionNet) != kVersion)
        {
            LOG_DEBUG << "proto: bad version, skip one candidate";
            SkipCurrentMagic(buf);
            if (++resync > kMaxResyncPerCall)
                return DecodeStatus::kError;
            continue;
        }

        uint32_t totalLenNet = 0;
        std::memcpy(&totalLenNet, buf.peek() + 26, sizeof(totalLenNet));
        const uint32_t totalLen = netToHost32(totalLenNet);

        // 长度必须能容纳一个头；否则 bodyLen 会下溢成约 4GB
        if (totalLen < kHeaderSize || totalLen > kMaxMessageSize)
        {
            LOG_DEBUG << "proto: illegal totalLen=" << totalLen << ", skip one candidate";
            SkipCurrentMagic(buf);
            if (++resync > kMaxResyncPerCall)
                return DecodeStatus::kError;
            continue;
        }

        const uint8_t dstServiceID = *reinterpret_cast<const uint8_t*>(buf.peek() + 31);

        // 过滤维度：这条消息是不是发给本服务的
        if (expectedDstServiceID != kServiceAny && dstServiceID != expectedDstServiceID)
        {
            LOG_DEBUG << "proto: dst=" << ServerIdName(dstServiceID)
                      << " not mine, skip one candidate";
            SkipCurrentMagic(buf);
            if (++resync > kMaxResyncPerCall)
                return DecodeStatus::kError;
            continue;
        }

        // 2. 整帧到齐了吗（半包）
        if (totalLen > buf.readableBytes())
            return DecodeStatus::kNeedMore;

        // 3. 消费帧头（逐字段 pop，顺序与编码严格一致）
        buf.readUint16();                       // magic（已校验）
        buf.readUint16();                       // version（已校验）
        head.msgType      = buf.readUint8();
        head.totalLen     = totalLen;
        head.retrFlag     = buf.readUint8();
        head.module       = buf.readUint16();
        head.method       = buf.readUint16();
        head.seq          = buf.readUint64();
        head.playerId     = buf.readUint64();
        buf.readUint32();                       // totalLen（整帧长度，已在上方校验并记录）
        head.srcServiceID = buf.readUint8();
        head.dstServiceID = buf.readUint8();

        // 注意区分两种长度：totalLen 是**整帧长度**，bodyLen 必须减去头长度。
        // 把 totalLen 直接当 bodyLen 用，会多吞 32 字节 -> 永远只解出半包。
        const uint32_t bodyLen = totalLen - kHeaderSize;
        if (buf.readableBytes() < bodyLen)
            return DecodeStatus::kNeedMore;

        body = buf.readAsString(bodyLen);
        return DecodeStatus::kOk;
    }
}
