#include "base/buffer.h"
#include "base/proto.h"
#include "base/utils.h"
#include "test_util.h"

#include <cstdint>
#include <cstring>
#include <string>

// 编解码是"人工推导偏移"的典型：一次字段增删就可能错位，
// 往返测试是唯一能自动发现这类错位的手段。
TEST(ProtoRoundTrip)
{
    const std::string body = "hello-protobuf";

    Buffer out;
    const Header h = MakeHeader(kSceneMsg, 3, 2, 42, 1001, kScene_1, kGateway);
    EncodeFrame(out, h, body);

    CHECK_EQ(out.readableBytes(), kHeaderSize + body.size());

    Header head;
    std::string got;
    const DecodeStatus st = DecodeFrame(out, head, kGateway, got);

    CHECK(st == DecodeStatus::kOk);
    CHECK_EQ(head.msgType, (uint8_t)kSceneMsg);
    CHECK_EQ(head.module, (uint16_t)3);
    CHECK_EQ(head.method, (uint16_t)2);
    CHECK_EQ(head.seq, (uint64_t)42);
    CHECK_EQ(head.playerId, (uint64_t)1001);
    CHECK_EQ(head.srcServiceID, (uint8_t)kScene_1);
    CHECK_EQ(head.dstServiceID, (uint8_t)kGateway);
    CHECK_EQ(head.totalLen, (uint32_t)(kHeaderSize + body.size()));
    CHECK(got == body);
    CHECK_EQ(out.readableBytes(), (size_t)0);
}

TEST(ProtoEmptyBody)
{
    Buffer out;
    EncodeFrame(out, MakeHeader(kGatewayMsg, 1, 1, 0, 0, kGateway, kScene_1), "");

    Header head;
    std::string body;
    CHECK(DecodeFrame(out, head, kScene_1, body) == DecodeStatus::kOk);
    CHECK_EQ(head.totalLen, (uint32_t)kHeaderSize);
    CHECK(body.empty());
}

// 粘包：两个帧连着到，必须能连续解出两条
TEST(ProtoStickyPackets)
{
    Buffer out;
    EncodeFrame(out, MakeHeader(kGatewayMsg, 3, 2, 1, 7, kGateway, kScene_1), "AAAA");
    EncodeFrame(out, MakeHeader(kGatewayMsg, 3, 3, 2, 7, kGateway, kScene_1), "BB");

    Header h1;
    std::string b1;
    CHECK(DecodeFrame(out, h1, kScene_1, b1) == DecodeStatus::kOk);
    CHECK(b1 == "AAAA");
    CHECK_EQ(h1.method, (uint16_t)2);

    Header h2;
    std::string b2;
    CHECK(DecodeFrame(out, h2, kScene_1, b2) == DecodeStatus::kOk);
    CHECK(b2 == "BB");
    CHECK_EQ(h2.method, (uint16_t)3);
    CHECK_EQ(h2.seq, (uint64_t)2);
}

// 半包：帧头到齐但 body 没齐 -> kNeedMore，且不得消费任何数据
TEST(ProtoHalfPacket)
{
    const std::string body = "0123456789";

    Buffer full;
    EncodeFrame(full, MakeHeader(kGatewayMsg, 3, 2, 1, 7, kGateway, kScene_1), body);

    // 只喂一半
    const size_t half = full.readableBytes() / 2;
    Buffer partial;
    partial.append(full.peek(), half);

    Header head;
    std::string got;
    CHECK(DecodeFrame(partial, head, kScene_1, got) == DecodeStatus::kNeedMore);
    CHECK_EQ(partial.readableBytes(), half);   // 一个字节都不能吞

    // 补齐后必须能解出来
    partial.append(full.peek() + half, full.readableBytes() - half);
    CHECK(DecodeFrame(partial, head, kScene_1, got) == DecodeStatus::kOk);
    CHECK(got == body);
}

// 脏数据重同步：全垃圾 + 垃圾前缀 + 伪魔数（本次不匹配的 dst）
TEST(ProtoResyncGarbageAndWrongDst)
{
    Buffer out;
    out.append("\x01\x02\x03\x04\x05", 5);   // 无魔数垃圾

    // 一条"发往别的服务"的帧：必须被跳过，不能当成自己的消息处理
    EncodeFrame(out, MakeHeader(kGatewayMsg, 9, 9, 0, 0, kScene_1, kDB), "OTHER");
    // 紧跟一条真正发往本服务的帧
    EncodeFrame(out, MakeHeader(kGatewayMsg, 3, 2, 5, 9, kGateway, kScene_1), "MINE");

    Header head;
    std::string body;
    CHECK(DecodeFrame(out, head, kScene_1, body) == DecodeStatus::kOk);
    CHECK(body == "MINE");
    CHECK_EQ(head.seq, (uint64_t)5);
}

// 非法 totalLen：必须被跳过而不是按"巨大 body"或"下溢的 bodyLen"去读
TEST(ProtoIllegalTotalLen)
{
    const std::string body = "AB";

    // ---- 情况 1：totalLen 小于头长度（旧代码会下溢成约 4GB）----
    {
        Buffer frame;
        EncodeFrame(frame, MakeHeader(kGatewayMsg, 3, 2, 5, 9, kGateway, kScene_1), body);

        std::string raw(frame.peek(), frame.readableBytes());
        raw[26] = 0;
        raw[27] = 0;
        raw[28] = 0;
        raw[29] = 5;   // totalLen = 5 < kHeaderSize

        Buffer in;
        in.append(raw.data(), raw.size());

        Header head;
        std::string got;
        // 伪帧被跳过；剩下的垃圾不足以构成一帧 -> kNeedMore
        CHECK(DecodeFrame(in, head, kScene_1, got) == DecodeStatus::kNeedMore);
        // 垃圾必须被清掉，不能无界积压
        CHECK(in.readableBytes() <= 1);
    }

    // ---- 情况 2：totalLen 超过上限 ----
    {
        Buffer frame;
        EncodeFrame(frame, MakeHeader(kGatewayMsg, 3, 2, 5, 9, kGateway, kScene_1), body);

        std::string raw(frame.peek(), frame.readableBytes());
        raw[26] = static_cast<char>(0x01);   // totalLen = 0x01FFFFFF = 33554431 > kMaxMessageSize
        raw[27] = static_cast<char>(0xFF);
        raw[28] = static_cast<char>(0xFF);
        raw[29] = static_cast<char>(0xFF);

        Buffer in;
        in.append(raw.data(), raw.size());

        Header head;
        std::string got;
        CHECK(DecodeFrame(in, head, kScene_1, got) == DecodeStatus::kNeedMore);
        CHECK(in.readableBytes() <= 1);
    }
}

// 魔数在偏移 0 且字段不匹配时不能死循环（旧实现在这里会 100% CPU 卡死网络线程）
TEST(ProtoNoInfiniteResync)
{
    Buffer in;
    // 造一堆"魔数正确、但 version 不对"的字节：每个候选都要被跳过
    for (int i = 0; i < 64; ++i)
    {
        std::string fake;
        // 魔数在帧里是大端字节序
        fake.push_back(static_cast<char>((kMagic >> 8) & 0xFF));
        fake.push_back(static_cast<char>(kMagic & 0xFF));
        for (int j = 0; j < 30; ++j)
            fake.push_back('x');
        in.append(fake.data(), fake.size());
    }

    Header head;
    std::string got;
    const DecodeStatus st = DecodeFrame(in, head, kServiceAny, got);
    // 要么认定协议错误，要么需要更多数据；关键是不能卡住、且缓冲要变小
    CHECK(st == DecodeStatus::kError || st == DecodeStatus::kNeedMore);
    CHECK(in.readableBytes() < 64u * 32u);
}
