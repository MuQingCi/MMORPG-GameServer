#include "base/clientProto.h"
#include "base/proto.h"
#include "test_util.h"

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// 客户端接入帧（ClientFrame）编解码
//
// 覆盖点（都是线上真会踩的）：
//   1. golden bytes：字段偏移与网络序一旦改动，这里立刻红；
//   2. 半包不消费、粘包按序解出；
//   3. 非法 totalLen / 版本不匹配 -> 跳过候选而不是死循环；
//   4. **两套协议互不误判**：把 GateFrame 喂给客户端解码器（反之亦然）绝不能解出 kOk。
// ---------------------------------------------------------------------------

namespace
{
// 把固定字节序列拼成 string（golden bytes 用，避免手写转义）
std::string Bytes(std::initializer_list<int> v)
{
    std::string s;
    for (int b : v)
        s.push_back(static_cast<char>(b & 0xFF));
    return s;
}
}  // namespace

TEST(ClientFrameGoldenBytes)
{
    Buffer out;
    ClientHeader h;
    h.kind = ClientKind::kRequest;
    h.module = 3;      // PLAYER
    h.method = 2;      // PLAYER_MOVE
    h.requestId = 0x0102030405060708ULL;
    EncodeClientFrame(out, h, "hi");

    // 头 20B + body 2B = 22
    const std::string expect = Bytes({
        0xC1, 0xEB,                                            // magic（大端）
        0x01,                                                  // version
        0x01,                                                  // kind = Request
        0x00, 0x03,                                            // module
        0x00, 0x02,                                            // method
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,        // requestId
        0x00, 0x00, 0x00, 0x16,                                // totalLen = 22
        'h',  'i',
    });

    CHECK_EQ(out.readableBytes(), expect.size());
    CHECK(std::string(out.peek(), out.readableBytes()) == expect);
}

TEST(ClientFrameDecodeRoundTrip)
{
    Buffer out;
    ClientHeader in;
    in.kind = ClientKind::kControl;
    in.module = kClientSysModule;
    in.method = ClientSysMethod::kAuth;
    in.requestId = 42;
    EncodeClientFrame(out, in, MakeAuthBody(1001, 998877));

    ClientHeader got;
    std::string body;
    CHECK(DecodeClientFrame(out, got, body) == DecodeStatus::kOk);
    CHECK(got.kind == ClientKind::kControl);
    CHECK_EQ(got.module, (uint16_t)kClientSysModule);
    CHECK_EQ(got.method, (uint16_t)ClientSysMethod::kAuth);
    CHECK_EQ(got.requestId, (uint64_t)42);
    CHECK_EQ(got.totalLen, (uint32_t)(kClientHeaderSize + 16));
    CHECK_EQ(body.size(), (size_t)16);

    uint64_t pid = 0;
    uint64_t ticket = 0;
    CHECK(ParseAuthBody(body, pid, ticket));
    CHECK_EQ(pid, (uint64_t)1001);
    CHECK_EQ(ticket, (uint64_t)998877);
    CHECK_EQ(out.readableBytes(), (size_t)0);
}

// 半包：不足一帧时不能消费任何字节（否则后续帧边界永久错乱）
TEST(ClientFrameHalfPacketNotConsumed)
{
    Buffer full;
    ClientHeader h;
    h.kind = ClientKind::kRequest;
    h.module = 3;
    h.method = 2;
    h.requestId = 7;
    EncodeClientFrame(full, h, "0123456789");

    const size_t total = full.readableBytes();
    Buffer part;
    part.append(full.peek(), 10);   // 只给 10 字节（头都不全）

    ClientHeader got;
    std::string body;
    CHECK(DecodeClientFrame(part, got, body) == DecodeStatus::kNeedMore);
    CHECK_EQ(part.readableBytes(), (size_t)10);

    // 补齐剩余字节后应能解出
    part.append(full.peek() + 10, total - 10);
    CHECK(DecodeClientFrame(part, got, body) == DecodeStatus::kOk);
    CHECK_EQ(got.requestId, (uint64_t)7);
    CHECK(body == "0123456789");
}

// 粘包：一次到达两帧，必须按序、按各自 requestId 解出
TEST(ClientFrameStickyPackets)
{
    Buffer buf;
    for (uint64_t i = 1; i <= 3; ++i)
    {
        ClientHeader h;
        h.kind = ClientKind::kRequest;
        h.module = 3;
        h.method = 2;
        h.requestId = 100 + i;
        EncodeClientFrame(buf, h, std::string("body") + std::to_string(i));
    }

    for (uint64_t i = 1; i <= 3; ++i)
    {
        ClientHeader got;
        std::string body;
        CHECK(DecodeClientFrame(buf, got, body) == DecodeStatus::kOk);
        CHECK_EQ(got.requestId, 100 + i);
        CHECK(body == std::string("body") + std::to_string(i));
    }
    CHECK_EQ(buf.readableBytes(), (size_t)0);
}

// 非法 totalLen：跳过这个候选、继续找下一帧（不能当成正常帧吞掉 4GB）
TEST(ClientFrameIllegalLengthSkipsCandidate)
{
    Buffer buf;
    // 伪造一帧：totalLen = 4（< 头长 20）
    const std::string bad = Bytes({
        0xC1, 0xEB, 0x01, 0x01, 0x00, 0x03, 0x00, 0x02,
        0, 0, 0, 0, 0, 0, 0, 1,
        0x00, 0x00, 0x00, 0x04,
    });
    buf.append(bad.data(), bad.size());

    ClientHeader good;
    good.kind = ClientKind::kRequest;
    good.module = 3;
    good.method = 2;
    good.requestId = 9;
    EncodeClientFrame(buf, good, "ok");

    ClientHeader got;
    std::string body;
    CHECK(DecodeClientFrame(buf, got, body) == DecodeStatus::kOk);
    CHECK_EQ(got.requestId, (uint64_t)9);
    CHECK(body == "ok");
}

// 版本不匹配：同样跳候选，且不会把缓冲吃光导致后续帧解不出
TEST(ClientFrameBadVersionSkipsCandidate)
{
    Buffer buf;
    const std::string bad = Bytes({
        0xC1, 0xEB, 0x02,        // version = 2（当前只支持 1）
        0x01, 0x00, 0x03, 0x00, 0x02,
        0, 0, 0, 0, 0, 0, 0, 1,
        0x00, 0x00, 0x00, 0x14,
    });
    buf.append(bad.data(), bad.size());

    ClientHeader good;
    good.kind = ClientKind::kPush;
    good.module = 3;
    good.method = 1;
    good.requestId = 5;
    EncodeClientFrame(buf, good, "");
    CHECK(buf.readableBytes() > bad.size());   // 脏数据后面确实还挂着一条好帧

    ClientHeader got;
    std::string body;
    CHECK(DecodeClientFrame(buf, got, body) == DecodeStatus::kOk);
    CHECK_EQ(got.requestId, (uint64_t)5);
    CHECK(got.kind == ClientKind::kPush);
}

// 纯垃圾：只保留可能的半截魔数，其余丢弃（有界，不无限积压）
TEST(ClientFrameGarbageDropped)
{
    Buffer buf;
    buf.append("GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27);

    ClientHeader got;
    std::string body;
    CHECK(DecodeClientFrame(buf, got, body) == DecodeStatus::kNeedMore);
    CHECK(buf.readableBytes() <= 1);   // 不能保留垃圾
}

// 超过客户端单帧上限的 totalLen 必须被拒绝（公网入口不能允许 1MB 帧）
TEST(ClientFrameTooLargeRejected)
{
    Buffer buf;
    const std::string bad = Bytes({
        0xC1, 0xEB, 0x01, 0x01, 0x00, 0x03, 0x00, 0x02,
        0, 0, 0, 0, 0, 0, 0, 1,
        0x00, 0x10, 0x00, 0x00,        // totalLen = 1MB > 64KB
    });
    buf.append(bad.data(), bad.size());

    ClientHeader got;
    std::string body;
    CHECK(DecodeClientFrame(buf, got, body) != DecodeStatus::kOk);
}

// ---------------------------------------------------------------------------
// "区分二者"的协议层守卫：两套帧格式互不误判
// ---------------------------------------------------------------------------
TEST(TwoProtocolsDoNotCrossDecode)
{
    // 1) 客户端帧喂给服务间解码器：绝不能解出 kOk
    Buffer clientFrame;
    ClientHeader ch;
    ch.kind = ClientKind::kRequest;
    ch.module = 3;
    ch.method = 2;
    ch.requestId = 1;
    EncodeClientFrame(clientFrame, ch, "hello");

    Header sh;
    std::string sbody;
    CHECK(DecodeFrame(clientFrame, sh, ServerID::kGateway, sbody) != DecodeStatus::kOk);

    // 注意这里**不**断言"缓冲一定被清空"：客户端帧只有 25 字节 < 服务间头长 32，
    // 解码器无法判断这是垃圾还是半包，因此它会原样等待（kNeedMore）。
    // 真正拦住"两套协议混用"的是网关的魔数交叉校验（见下 + e2e 用例 1.5/1.6）。
    // 补足到 ≥32 字节后，服务间解码器就会把它当垃圾丢掉。
    clientFrame.append(std::string(16, 'x').data(), 16);
    CHECK(DecodeFrame(clientFrame, sh, ServerID::kGateway, sbody) != DecodeStatus::kOk);
    CHECK(clientFrame.readableBytes() <= 1);

    // 2) 服务间帧喂给客户端解码器：同样不能解出 kOk，且脏数据被丢弃
    Buffer gateFrame;
    EncodeFrame(gateFrame,
                MakeHeader(NetMessageType::kSceneMsg, 3, 2, 1, 1001, ServerID::kScene_1, ServerID::kGateway),
                "hello");

    ClientHeader got;
    std::string cbody;
    CHECK(DecodeClientFrame(gateFrame, got, cbody) != DecodeStatus::kOk);
    CHECK(gateFrame.readableBytes() <= 1);

    // 3) 两套协议的魔数互不相同（网关靠它做端口级交叉校验）
    CHECK(kMagic != kClientMagic);
}

// 定长控制体的边界：短包必须被拒（不能读越界）
TEST(ClientControlBodyBounds)
{
    uint64_t pid = 0, sid = 0, ticket = 0;
    uint32_t epoch = 0, zone = 0;
    uint8_t dst = 0;

    CHECK(!ParseAuthBody(std::string(7, 'x'), pid, ticket));
    CHECK(!ParseAuthBody(MakeAuthBody(1, 2) + "x", pid, ticket));
    CHECK(!ParseAuthAckBody(std::string(24, 'x'), pid, sid, epoch, zone, dst));

    const std::string ack = MakeAuthAckBody(1001, 77, 3, 1, ServerID::kScene_1);
    CHECK_EQ(ack.size(), (size_t)25);
    CHECK(ParseAuthAckBody(ack, pid, sid, epoch, zone, dst));
    CHECK_EQ(pid, (uint64_t)1001);
    CHECK_EQ(sid, (uint64_t)77);
    CHECK_EQ(epoch, (uint32_t)3);
    CHECK_EQ(zone, (uint32_t)1);
    CHECK_EQ(dst, (uint8_t)ServerID::kScene_1);
    CHECK(!ParseAuthAckBody(ack + "x", pid, sid, epoch, zone, dst));

    uint8_t svc = 0;
    CHECK(!ParseServiceBody("", svc));
    CHECK(!ParseServiceBody("\x08\x09", svc));
    CHECK(ParseServiceBody(MakeServiceBody(8), svc));
    CHECK_EQ(svc, (uint8_t)8);
}

// 服务间握手体：与场景服既有小端布局必须完全一致（9 字节）
TEST(BackendHandshakeCodec)
{
    BackendHandshake hs;
    hs.serviceId = ServerID::kScene_1;
    hs.zoneId = 0x01020304;
    hs.sceneId = 1;

    const std::string body = PackBackendHandshake(hs);
    CHECK_EQ(body.size(), (size_t)9);
    CHECK_EQ((uint8_t)body[0], (uint8_t)8);
    CHECK_EQ((uint8_t)body[1], (uint8_t)0x04);   // 小端
    CHECK_EQ((uint8_t)body[4], (uint8_t)0x01);

    BackendHandshake out;
    CHECK(UnpackBackendHandshake(body, out));
    CHECK_EQ(out.serviceId, (uint8_t)ServerID::kScene_1);
    CHECK_EQ(out.zoneId, (uint32_t)0x01020304);
    CHECK_EQ(out.sceneId, (uint32_t)1);

    CHECK(!UnpackBackendHandshake(std::string(8, 'x'), out));   // 短包拒绝
    CHECK(!UnpackBackendHandshake(body + "x", out));             // 尾随字节拒绝
}
