#include "e2e_conn.h"
#include "test_util.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

// ============================================================================
// 网关端到端（进程级）：本工具扮演**一个场景服 + 多个客户端**，
// 对着真实运行的 gatewayServer 进程验证"两类数据都能收发，且被严格区分"。
//
// 端口来自环境变量（由 smoke_gateway_e2e.sh 注入）：
//   GW_PUBLIC_PORT  —— 公网端口（客户端链路，ClientFrame）
//   GW_PRIVATE_PORT —— 内网端口（区服服务器链路，GateFrame）
// 票据与服务号必须与脚本写入的临时配置一致（clientToken / allowedServices）。
//
// 与单测的分工：单测覆盖编解码与三张表的语义；这里覆盖"真进程 + 真 socket +
// 两类链路同时在线"的交互，尤其是 fail-closed 的那些分支。
// ============================================================================

namespace
{
constexpr uint16_t kModPlayer        = 3;   // 与 sceneServer 的 Module::PLAYER 对齐
constexpr uint16_t kMethodMove       = 2;   // Method::PLAYER_MOVE
constexpr uint16_t kMethodPushState  = 1;   // Method::PLAYER_PUSH_STATE（仅出站推送）
constexpr uint64_t kClientToken      = 998877;   // 与临时配置的 gateway.clientToken 一致

uint16_t EnvPort(const char* name)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0')
        return 0;
    return static_cast<uint16_t>(std::atoi(v));
}

uint16_t PublicPort()  { return EnvPort("GW_PUBLIC_PORT"); }
uint16_t PrivatePort() { return EnvPort("GW_PRIVATE_PORT"); }

bool EnvReady()
{
    if (PublicPort() == 0 || PrivatePort() == 0)
    {
        std::fprintf(stderr, "[e2e] 缺少 GW_PUBLIC_PORT / GW_PRIVATE_PORT（应由 smoke 脚本注入）\n");
        return false;
    }
    return true;
}

// 鉴权并校验 AuthAck
bool AuthClient(e2e::Conn& c, uint64_t playerId, uint64_t ticket, uint32_t& epoch,
                uint8_t& dstService, uint64_t& sessionId)
{
    if (!c.sendClientFrame(ClientKind::kControl, kClientSysModule, ClientSysMethod::kAuth,
                           /*requestId=*/1, MakeAuthBody(playerId, ticket)))
        return false;

    ClientHeader h;
    std::string body;
    if (!c.readClientFrame(h, body, 1000))
        return false;
    if (h.kind != ClientKind::kControl || h.method != ClientSysMethod::kAuthAck || h.requestId != 1)
        return false;

    uint64_t pid = 0;
    uint32_t zone = 0;
    if (!ParseAuthAckBody(body, pid, sessionId, epoch, zone, dstService))
        return false;
    return pid == playerId;
}

// 场景服侧握手：上报服务身份（这是后端唯一的注册入口）
bool SendBackendHandshake(e2e::Conn& c, uint8_t serviceId, uint32_t zoneId, uint32_t sceneId)
{
    BackendHandshake hs;
    hs.serviceId = serviceId;
    hs.zoneId = zoneId;
    hs.sceneId = sceneId;
    return c.sendGateFrame(NetMessageType::kSceneMsg, SysModule::kSYS, SysMethod::kHandshake,
                           /*seq=*/0, /*playerId=*/0, serviceId, ServerID::kGateway,
                           PackBackendHandshake(hs));
}
}  // namespace

// ---------------------------------------------------------------------------
// 1) 客户端链路的控制通道与 fail-closed 守卫
// ---------------------------------------------------------------------------
TEST(E2eClientGuardsAndControlChannel)
{
    if (!EnvReady())
    {
        CHECK(false);
        return;
    }

    // ---- 1.1 未鉴权就发业务请求：必须拒绝（而不是转发）----
    {
        e2e::Conn c;
        CHECK(c.open(PublicPort()));
        CHECK(c.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 11, "move"));
        ClientHeader h;
        std::string body;
        CHECK(c.readClientFrame(h, body, 1000));
        CHECK(h.kind == ClientKind::kError);
        CHECK_EQ(h.method, (uint16_t)kMethodMove);
        CHECK_EQ(h.requestId, (uint64_t)11);
        CHECK(body.find("not authenticated") != std::string::npos);
    }

    // ---- 1.2 心跳在鉴权前后都可用 ----
    {
        e2e::Conn c;
        CHECK(c.open(PublicPort()));
        CHECK(c.sendClientFrame(ClientKind::kControl, kClientSysModule, ClientSysMethod::kPing, 12, ""));
        ClientHeader h;
        std::string body;
        CHECK(c.readClientFrame(h, body, 1000));
        CHECK(h.kind == ClientKind::kControl);
        CHECK_EQ(h.method, (uint16_t)ClientSysMethod::kPong);
        CHECK_EQ(h.requestId, (uint64_t)12);
    }

    // ---- 1.3 票据错误：回 AuthFail 后必须断开（否则公网可以无限试票据）----
    {
        e2e::Conn c;
        CHECK(c.open(PublicPort()));
        CHECK(c.sendClientFrame(ClientKind::kControl, kClientSysModule, ClientSysMethod::kAuth, 13,
                                MakeAuthBody(1001, kClientToken + 1)));
        ClientHeader h;
        std::string body;
        CHECK(c.readClientFrame(h, body, 1000));
        CHECK(h.kind == ClientKind::kError);
        CHECK_EQ(h.method, (uint16_t)ClientSysMethod::kAuthFail);
        CHECK(c.expectClosedByPeer(1000));
    }

    // ---- 1.4 鉴权成功：拿到 session/epoch/dstService，且客户端不能自造 Response ----
    {
        e2e::Conn c;
        CHECK(c.open(PublicPort()));
        uint32_t epoch = 0;
        uint8_t dst = 0;
        uint64_t sid = 0;
        CHECK(AuthClient(c, 1001, kClientToken, epoch, dst, sid));
        CHECK(sid != 0);
        CHECK_EQ(epoch, (uint32_t)1);
        CHECK_EQ(dst, (uint8_t)ServerID::kScene_1);

        CHECK(c.sendClientFrame(ClientKind::kResponse, kModPlayer, kMethodMove, 14, "fake"));
        ClientHeader h;
        std::string body;
        CHECK(c.readClientFrame(h, body, 1000));
        CHECK(h.kind == ClientKind::kError);
        CHECK(body.find("only Request kind") != std::string::npos);
    }

    // ---- 1.5 公网端口上发服务间帧（0xC1EA）：连接必须被断开 ----
    {
        e2e::Conn c;
        CHECK(c.open(PublicPort()));
        CHECK(c.sendClientFrame(ClientKind::kControl, kModPlayer, ClientSysMethod::kPing, 15, ""));
        ClientHeader h;
        std::string body;
        CHECK(c.readClientFrame(h, body, 1000));
        CHECK(h.kind == ClientKind::kError);
        CHECK(body.find("control kind and module mismatch") != std::string::npos);
        CHECK(c.sendClientFrame(ClientKind::kRequest, kClientSysModule, kMethodMove, 16, ""));
        CHECK(c.readClientFrame(h, body, 1000));
        CHECK(h.kind == ClientKind::kError);
        CHECK(body.find("control kind and module mismatch") != std::string::npos);
        CHECK(c.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 0, ""));
        CHECK(c.readClientFrame(h, body, 1000));
        CHECK(h.kind == ClientKind::kError);
        CHECK(body.find("requestId must be nonzero") != std::string::npos);
    }

    // ---- 1.6 公网端口上发服务间帧（0xC1EA）：连接必须被断开 ----
    {
        e2e::Conn c;
        CHECK(c.open(PublicPort()));
        CHECK(c.sendGateFrame(NetMessageType::kSceneMsg, kModPlayer, kMethodMove, 1, 1001,
                              ServerID::kScene_1, ServerID::kGateway, "wrong-proto"));
        CHECK(c.expectClosedByPeer(1000));
    }

    // ---- 1.7 内网端口上发客户端帧（0xC1EB）：同样必须被断开 ----
    {
        e2e::Conn b;
        CHECK(b.open(PrivatePort()));
        CHECK(b.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 1, "wrong-proto"));
        CHECK(b.expectClosedByPeer(1000));
    }
}

// ---------------------------------------------------------------------------
// 2) 后端未认证 / 未注册时的行为
// ---------------------------------------------------------------------------
TEST(E2eBackendMustRegisterBeforeBusiness)
{
    if (!EnvReady())
    {
        CHECK(false);
        return;
    }

    // ---- 2.1 未握手就发业务帧：连接必须被断开 ----
    {
        e2e::Conn b;
        CHECK(b.open(PrivatePort()));
        CHECK(b.sendGateFrame(NetMessageType::kSceneMsg, kModPlayer, kMethodMove, 1, 1001,
                              ServerID::kScene_1, ServerID::kGateway, "unregistered"));
        CHECK(b.expectClosedByPeer(1000));
    }

    // ---- 2.2 没有可用后端时，客户端请求必须收到明确错误（而不是静默丢弃/挂住）----
    {
        e2e::Conn c;
        CHECK(c.open(PublicPort()));
        uint32_t epoch = 0;
        uint8_t dst = 0;
        uint64_t sid = 0;
        CHECK(AuthClient(c, 2001, kClientToken, epoch, dst, sid));

        CHECK(c.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 21, "move"));
        ClientHeader h;
        std::string body;
        CHECK(c.readClientFrame(h, body, 1000));
        CHECK(h.kind == ClientKind::kError);
        CHECK(body.find("unavailable") != std::string::npos);
        CHECK_EQ(h.requestId, (uint64_t)21);
    }
}

// ---------------------------------------------------------------------------
// 3) 完整链路：客户端 -> 网关 -> 场景服 -> 网关 -> 客户端
//    重点验证"一条后端连接复用多个客户端、且不串包"
// ---------------------------------------------------------------------------
TEST(E2eFullPathAndNoCrossTalk)
{
    if (!EnvReady())
    {
        CHECK(false);
        return;
    }

    // ---- 3.1 场景服上线：连内网端口 + 握手（不发握手 ack，注册成功由后续转发证明）----
    auto backend = std::make_unique<e2e::Conn>();
    CHECK(backend->open(PrivatePort()));
    CHECK(SendBackendHandshake(*backend, ServerID::kScene_1, /*zoneId=*/1, /*sceneId=*/1));

    // ---- 3.2 两个客户端鉴权：**playerId 由网关绑定** ----
    e2e::Conn ca;
    e2e::Conn cb;
    CHECK(ca.open(PublicPort()));
    CHECK(cb.open(PublicPort()));

    uint32_t epochA = 0, epochB = 0;
    uint8_t dstA = 0, dstB = 0;
    uint64_t sidA = 0, sidB = 0;
    CHECK(AuthClient(ca, 1001, kClientToken, epochA, dstA, sidA));
    CHECK(AuthClient(cb, 1002, kClientToken, epochB, dstB, sidB));
    CHECK(sidA != sidB);

    // ---- 3.3 两个客户端用**相同 requestId**（77）发请求 ----
    CHECK(ca.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 77, "move:1001"));
    CHECK(cb.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 77, "move:1002"));

    // ---- 3.4 场景服侧应收到两条转发帧：dst=SCENE_1、src=GATEWAY、playerId=绑定值、seq 唯一 ----
    uint64_t seqFor[2] = {0, 0};
    uint64_t pidFor[2] = {0, 0};
    std::string bodyFor[2];
    for (int i = 0; i < 2; ++i)
    {
        Header h;
        std::string body;
        CHECK(backend->readGateFrame(h, body, 2000));
        CHECK_EQ(h.dstServiceID, (uint8_t)ServerID::kScene_1);
        CHECK_EQ(h.srcServiceID, (uint8_t)ServerID::kGateway);
        CHECK_EQ(h.msgType, (uint8_t)NetMessageType::kSceneMsg);
        CHECK_EQ(h.module, (uint16_t)kModPlayer);
        CHECK_EQ(h.method, (uint16_t)kMethodMove);
        CHECK(h.seq != 0);
        seqFor[i] = h.seq;
        pidFor[i] = h.playerId;
        bodyFor[i] = body;
    }
    CHECK(seqFor[0] != seqFor[1]);                 // 网关内部号唯一 = 不串包的前提
    CHECK(pidFor[0] != pidFor[1]);                 // 两个客户端身份不同
    CHECK(pidFor[0] == 1001 || pidFor[0] == 1002);
    CHECK(pidFor[1] == 1001 || pidFor[1] == 1002);
    CHECK(bodyFor[0] != bodyFor[1]);               // 两条请求体没有互相覆盖

    // ---- 3.5 场景服按原 seq/playerId 回包：网关必须还原成各自客户端自己的 requestId ----
    for (int i = 0; i < 2; ++i)
    {
        const std::string ack = "ack:" + std::to_string(pidFor[i]);
        CHECK(backend->sendGateFrame(NetMessageType::kSceneMsg, kModPlayer, kMethodMove, seqFor[i],
                                     pidFor[i], ServerID::kScene_1, ServerID::kGateway, ack));
    }

    for (int i = 0; i < 2; ++i)
    {
        e2e::Conn& c = (pidFor[i] == 1001) ? ca : cb;
        ClientHeader h;
        std::string body;
        CHECK(c.readClientFrame(h, body, 2000));
        CHECK(h.kind == ClientKind::kResponse);
        CHECK_EQ(h.requestId, (uint64_t)77);       // 客户端看到的仍是自己的 requestId
        CHECK_EQ(h.module, (uint16_t)kModPlayer);
        CHECK_EQ(h.method, (uint16_t)kMethodMove);
        CHECK(body == "ack:" + std::to_string(pidFor[i]));
    }

    // ---- 3.6 主动推送：无对应请求、只带 playerId -> 送到该玩家**当前**会话 ----
    CHECK(backend->sendGateFrame(NetMessageType::kSceneMsg, kModPlayer, kMethodPushState, /*seq=*/0,
                                 1002, ServerID::kScene_1, ServerID::kGateway, "push-for-1002"));
    {
        ClientHeader h;
        std::string body;
        CHECK(cb.readClientFrame(h, body, 2000));
        CHECK(h.kind == ClientKind::kPush);
        CHECK_EQ(h.requestId, (uint64_t)0);        // 推送没有请求号
        CHECK(body == "push-for-1002");
    }
    {
        // 另一个客户端不能收到这条推送
        ClientHeader h;
        std::string body;
        CHECK(!ca.readClientFrame(h, body, 300));
    }

    // 非零但未知的 seq 是迟到/重复响应，不得降级为 Push 送给当前玩家。
    CHECK(backend->sendGateFrame(NetMessageType::kSceneMsg, kModPlayer, kMethodMove,
                                 seqFor[0], pidFor[0], ServerID::kScene_1, ServerID::kGateway,
                                 "duplicate-response"));
    {
        ClientHeader h;
        std::string body;
        CHECK(!ca.readClientFrame(h, body, 300));
        CHECK(!cb.readClientFrame(h, body, 300));
    }

    // ---- 3.7 场景服断开：注册必须被摘除，客户端请求转为明确错误 ----
    // 注意这里有**真实竞态**：FIN 发出后，网关要等自己的事件循环处理到 EOF 才会摘除注册，
    // 这段窗口内的请求仍会被转发（然后石沉大海，因为对端已经没了）。
    // 所以用"有界重试观察状态切换"来断言，而不是 sleep 一个拍脑袋的时长。
    backend.reset();   // 关闭到网关的内网连接
    bool gotUnavailable = false;
    for (int attempt = 0; attempt < 20 && !gotUnavailable; ++attempt)
    {
        CHECK(ca.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 100 + attempt, "after"));
        ClientHeader h;
        std::string body;
        if (ca.readClientFrame(h, body, 200))
        {
            if (h.kind == ClientKind::kError && body.find("unavailable") != std::string::npos)
                gotUnavailable = true;
        }
        if (!gotUnavailable)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(gotUnavailable);
}

// ---------------------------------------------------------------------------
// 已登记连接必须如实上报来源；伪造回包不得消费其它请求的路由。
// ---------------------------------------------------------------------------
TEST(E2eBackendResponseContextGuard)
{
    if (!EnvReady())
    {
        CHECK(false);
        return;
    }

    e2e::Conn client;
    CHECK(client.open(PublicPort()));
    uint32_t epoch = 0;
    uint8_t dst = 0;
    uint64_t sid = 0;
    CHECK(AuthClient(client, 4001, kClientToken, epoch, dst, sid));

    auto backend = std::make_unique<e2e::Conn>();
    CHECK(backend->open(PrivatePort()));
    CHECK(SendBackendHandshake(*backend, ServerID::kScene_1, 1, 1));
    Header req;
    std::string body;
    // 握手没有 ACK；用有界轮询等待注册完成，避免异步事件循环的时间竞态。
    bool routed = false;
    for (int attempt = 0; attempt < 20 && !routed; ++attempt)
    {
        CHECK(client.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 42, "request"));
        if (backend->readGateFrame(req, body, 100))
            routed = true;
        else
        {
            ClientHeader err;
            CHECK(client.readClientFrame(err, body, 1000));
            CHECK(err.kind == ClientKind::kError);
        }
    }
    CHECK(routed);
    CHECK(req.seq != 0);
    CHECK_EQ(req.playerId, (uint64_t)4001);

    // 连接已登记为 Scene_1，帧头却声称来自 DB：应断连，但不能消费 seq。
    CHECK(backend->sendGateFrame(NetMessageType::kSceneMsg, kModPlayer, kMethodMove,
                                 req.seq, req.playerId, ServerID::kDB, ServerID::kGateway, "spoof"));
    CHECK(backend->expectClosedByPeer(1000));
    backend.reset();
    backend = std::make_unique<e2e::Conn>();
    CHECK(backend->open(PrivatePort()));
    CHECK(SendBackendHandshake(*backend, ServerID::kScene_1, 1, 1));

    // 正确服务号但错误玩家/方法，同样不能抢占尚未消费的路由。
    CHECK(backend->sendGateFrame(NetMessageType::kSceneMsg, kModPlayer, kMethodMove,
                                 req.seq, 4002, ServerID::kScene_1, ServerID::kGateway, "wrong-player"));
    CHECK(backend->expectClosedByPeer(1000));
    backend.reset();
    backend = std::make_unique<e2e::Conn>();
    CHECK(backend->open(PrivatePort()));
    CHECK(SendBackendHandshake(*backend, ServerID::kScene_1, 1, 1));
    CHECK(backend->sendGateFrame(NetMessageType::kSceneMsg, kModPlayer, kMethodMove,
                                 req.seq, req.playerId, ServerID::kScene_1, ServerID::kGateway, "valid"));
    ClientHeader response;
    CHECK(client.readClientFrame(response, body, 2000));
    CHECK(response.kind == ClientKind::kResponse);
    CHECK_EQ(response.requestId, (uint64_t)42);
    CHECK(body == "valid");
}

// ---------------------------------------------------------------------------
// 4) 顶号：同一 playerId 的新会话必须把旧会话踢下线
// ---------------------------------------------------------------------------
TEST(E2eKickOldSessionOnRelogin)
{
    if (!EnvReady())
    {
        CHECK(false);
        return;
    }

    e2e::Conn oldC;
    e2e::Conn newC;
    CHECK(oldC.open(PublicPort()));
    CHECK(newC.open(PublicPort()));

    uint32_t epoch = 0;
    uint8_t dst = 0;
    uint64_t sid = 0;
    CHECK(AuthClient(oldC, 3001, kClientToken, epoch, dst, sid));
    CHECK_EQ(epoch, (uint32_t)1);

    CHECK(AuthClient(newC, 3001, kClientToken, epoch, dst, sid));
    CHECK_EQ(epoch, (uint32_t)2);            // 同一玩家的 epoch 递增

    // 旧连接必须被网关关闭
    CHECK(oldC.expectClosedByPeer(1000));

    // 新会话仍然有效：错误是"后端不可用"而不是"未鉴权"
    CHECK(newC.sendClientFrame(ClientKind::kRequest, kModPlayer, kMethodMove, 31, "still-alive"));
    ClientHeader h;
    std::string body;
    CHECK(newC.readClientFrame(h, body, 1000));
    CHECK(h.kind == ClientKind::kError);
    CHECK(body.find("unavailable") != std::string::npos);
}
