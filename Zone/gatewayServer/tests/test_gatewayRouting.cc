#include "clientSession.h"
#include "returnRoute.h"
#include "serviceRegistry.h"
#include "test_util.h"

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// 网关三张表的语义（都在"多客户端复用一条后端连接"这条链路上）：
//   ClientSessionTable : 会话身份（playerId / epoch / 顶号）
//   ServiceRegistry    : 后端节点身份（握手认证后的 <serviceId> -> 连接）
//   ReturnRouteTable   : 回程路由（internalSeq -> 客户端请求），必须有界且能回收
//
// 表里存的是连接对象（TcpConnectionPtr）；这些用例只关心表语义，
// 因此传 nullptr —— 不需要真起网络就能覆盖索引与生命周期规则。
// ---------------------------------------------------------------------------

// ---------------- 客户端会话表 ----------------
TEST(ClientSessionCreateAndBind)
{
    ClientSessionTable t;

    const uint64_t sid = t.Create(nullptr, 8, 1000);
    CHECK(sid != 0);
    CHECK_EQ(t.Size(), (size_t)1);

    ClientSession s;
    CHECK(t.Get(sid, s));
    CHECK(!s.authed);
    CHECK_EQ(s.playerId, (uint64_t)0);
    CHECK_EQ(s.dstService, (uint8_t)8);
    CHECK_EQ(s.epoch, (uint32_t)0);

    uint64_t kicked = 0;
    CHECK(t.Bind(sid, 1001, 8, 2000, kicked));
    CHECK_EQ(kicked, (uint64_t)0);

    CHECK(t.Get(sid, s));
    CHECK(s.authed);
    CHECK_EQ(s.playerId, (uint64_t)1001);
    CHECK_EQ(s.epoch, (uint32_t)1);

    // 玩家索引：后端推送只带 playerId，必须能定位到会话
    ClientSession byPlayer;
    CHECK(t.FindByPlayer(1001, byPlayer));
    CHECK_EQ(byPlayer.sessionId, sid);
    CHECK(!t.FindByPlayer(1002, byPlayer));
    CHECK(!t.FindByPlayer(0, byPlayer));

    // 绑定到不存在的会话必须失败（连接已断的竞态）
    CHECK(!t.Bind(99999, 1001, 8, 2000, kicked));
    CHECK(!t.SetDstService(99999, 3));
    CHECK(t.SetDstService(sid, 3));
    CHECK(t.Get(sid, s));
    CHECK_EQ(s.dstService, (uint8_t)3);
}

// 顶号：同一玩家新会话鉴权时，旧会话必须被踢出玩家索引并失效
TEST(ClientSessionKickOldSession)
{
    ClientSessionTable t;

    const uint64_t oldSid = t.Create(nullptr, 8, 1000);
    const uint64_t newSid = t.Create(nullptr, 8, 1100);

    uint64_t kicked = 0;
    CHECK(t.Bind(oldSid, 1001, 8, 1200, kicked));
    CHECK_EQ(kicked, (uint64_t)0);

    CHECK(t.Bind(newSid, 1001, 8, 1300, kicked));
    CHECK_EQ(kicked, oldSid);              // 旧会话被顶掉

    ClientSession s;
    CHECK(t.Get(oldSid, s));
    CHECK(!s.authed);                      // 旧会话降级：不能再发业务请求
    CHECK_EQ(s.playerId, (uint64_t)0);

    CHECK(t.Get(newSid, s));
    CHECK_EQ(s.epoch, (uint32_t)2);        // epoch 递增：旧回包据此被丢弃

    ClientSession byPlayer;
    CHECK(t.FindByPlayer(1001, byPlayer));
    CHECK_EQ(byPlayer.sessionId, newSid);

    // 旧会话随后被关闭：不能把新会话的玩家索引一起删掉
    CHECK(t.Erase(oldSid));
    CHECK(t.FindByPlayer(1001, byPlayer));
    CHECK_EQ(byPlayer.sessionId, newSid);

    CHECK(!t.Erase(oldSid));              // 重复 Erase 幂等
}

// ---------------- 后端服务注册表 ----------------
TEST(ServiceRegistryPendingThenBind)
{
    ServiceRegistry r;

    CHECK(r.AddPending("ClearMoon-b#1", nullptr));
    CHECK_EQ(r.Size(), (size_t)1);
    CHECK_EQ(r.ReadyCount(), (size_t)0);   // 未握手 = 未认证

    ServiceEndpoint ep;
    CHECK(r.GetByName("ClearMoon-b#1", ep));
    CHECK_EQ(ep.serviceId, (uint8_t)0);    // 认证前没有身份

    // 不是本网关 accept 出来的连接：拒绝绑定
    CHECK(!r.Bind("ClearMoon-b#404", 8, 1, 1, 1000, nullptr));

    CHECK(r.Bind("ClearMoon-b#1", 8, 1, 1, 1000, nullptr));
    CHECK_EQ(r.ReadyCount(), (size_t)1);
    CHECK(r.GetByService(8, ep));
    CHECK_EQ(ep.connName, std::string("ClearMoon-b#1"));
    CHECK_EQ(ep.zoneId, (uint32_t)1);
    CHECK_EQ(ep.sceneId, (uint32_t)1);
    CHECK(!r.GetByService(3, ep));          // 别的服务号没有节点
}

// 同号节点重连：后到者替换，旧连接的记录降级（且旧连接关闭时不能摘掉新连接的索引）
TEST(ServiceRegistryReRegisterReplacesOld)
{
    ServiceRegistry r;

    CHECK(r.AddPending("b-old", nullptr));
    CHECK(r.AddPending("b-new", nullptr));

    std::string oldConn;
    CHECK(r.Bind("b-old", 8, 1, 1, 1000, &oldConn));
    CHECK(oldConn.empty());

    CHECK(r.Bind("b-new", 8, 1, 2, 2000, &oldConn));
    CHECK_EQ(oldConn, std::string("b-old"));   // 调用方据此关闭旧连接

    ServiceEndpoint ep;
    CHECK(r.GetByService(8, ep));
    CHECK_EQ(ep.connName, std::string("b-new"));

    // 旧连接关闭：不能把新连接的服务索引一起摘掉
    ServiceEndpoint removed;
    CHECK(r.RemoveByName("b-old", removed));
    CHECK_EQ(removed.serviceId, (uint8_t)0);
    CHECK(r.GetByService(8, ep));
    CHECK_EQ(ep.connName, std::string("b-new"));

    // 新连接关闭：索引随之消失
    CHECK(r.RemoveByName("b-new", removed));
    CHECK_EQ(removed.serviceId, (uint8_t)8);
    CHECK(!r.GetByService(8, ep));
    CHECK_EQ(r.ReadyCount(), (size_t)0);
    CHECK(!r.RemoveByName("b-new", removed));  // 重复移除幂等
}

// ---------------- 回程路由表 ----------------
namespace
{
ReturnRoute MakeRoute(uint64_t seq, uint64_t sid, uint64_t reqId, int64_t nowMs)
{
    ReturnRoute r;
    r.internalSeq = seq;
    r.clientSessionId = sid;
    r.clientRequestId = reqId;
    r.sessionEpoch = 1;
    r.createdAtMs = nowMs;
    return r;
}
}  // namespace

TEST(ReturnRouteAddTake)
{
    ReturnRouteTable t;

    CHECK(!t.Add(MakeRoute(0, 1, 1, 0), 100));       // internalSeq 0 非法
    CHECK(t.Add(MakeRoute(1, 10, 500, 1000), 100));
    CHECK(t.Add(MakeRoute(2, 11, 500, 1000), 100));  // 两客户端同 requestId：互不影响
    CHECK_EQ(t.Size(), (size_t)2);

    ReturnRoute r;
    CHECK(t.Get(1, r));
    CHECK_EQ(t.Size(), (size_t)2);                 // 来源验证前不能消费
    CHECK(t.Take(1, r));
    CHECK_EQ(r.clientSessionId, (uint64_t)10);
    CHECK_EQ(r.clientRequestId, (uint64_t)500);
    CHECK(!t.Take(1, r));                            // 已消费
    CHECK(!t.Get(1, r));
    CHECK(t.Take(2, r));
    CHECK_EQ(r.clientSessionId, (uint64_t)11);
    CHECK_EQ(t.Size(), (size_t)0);
}

// 有界：满了必须拒绝，而不是让表无限增长
TEST(ReturnRouteBounded)
{
    ReturnRouteTable t;

    CHECK(t.Add(MakeRoute(1, 10, 1, 0), 2));
    CHECK(t.Add(MakeRoute(2, 10, 2, 0), 2));
    CHECK(!t.Add(MakeRoute(3, 10, 3, 0), 2));   // 超容量 -> false（调用方回错误）
    CHECK_EQ(t.Size(), (size_t)2);
}

// 会话关闭与 TTL：在途记录必须能被回收（否则慢后端会把表撑满）
TEST(ReturnRouteEraseBySessionAndSweep)
{
    ReturnRouteTable t;

    CHECK(t.Add(MakeRoute(1, 10, 1, 1000), 100));
    CHECK(t.Add(MakeRoute(2, 10, 2, 1000), 100));
    CHECK(t.Add(MakeRoute(3, 20, 3, 1000), 100));

    CHECK_EQ(t.EraseBySession(10), (size_t)2);
    CHECK_EQ(t.Size(), (size_t)1);

    // TTL 15s：14.9s 时不回收，15.1s 时回收
    CHECK_EQ(t.Sweep(1000 + 14900, 15000), (size_t)0);
    CHECK_EQ(t.Size(), (size_t)1);
    CHECK_EQ(t.Sweep(1000 + 15100, 15000), (size_t)1);
    CHECK_EQ(t.Size(), (size_t)0);
}
