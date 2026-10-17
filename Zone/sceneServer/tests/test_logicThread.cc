#include "common/msg.h"
#include "common/msgBus.h"
#include "logic/logicThread.h"
#include "metrics/sceneMetrics.h"
#include "test_util.h"
#include "gameProto.pb.h"

#include <chrono>
#include <thread>
#include <utility>
#include <stdexcept>

// ---------------------------------------------------------------------------
// LogicThread 消息分发顺序（阶段 1 任务 1.16 / 1.18 的回归守卫）
//
// 测法：只靠**真实线程 + 真实消息投递**，不用任何桩：
//   - 构造一个 luaDir 指向不存在目录的 LogicThread（Lua 未就绪）；
//   - 通过 MsgBus::sendToWorker 投消息，等价于网络线程的投递路径；
//   - 用 playerCount() 观察 Actor 是否被创建（1.18 之后它是原子快照，跨线程读安全）。
//
// 为什么不依赖 Lua 脚本：1.16 的落点是"**路由存在**就创建 Actor"（见 onNetMsg 注释），
// 所以"合法路由"用例在 Lua 未就绪时同样会创建 Actor —— 用例稳定、不需要脚本环境。
// ---------------------------------------------------------------------------
namespace
{
LogicThread::Config TestCfg()
{
    LogicThread::Config cfg;
    cfg.luaDir = "/nonexistent_lua_dir_used_by_unit_test";
    cfg.idleWaitMs = 5;
    cfg.flushIntervalMs = 600000;   // 抬高：避免测试期间触发周期落盘
    cfg.scanIdleMs = 600000;        // 抬高：避免空闲扫描干扰
    cfg.luaInstructionLimit = 0;
    cfg.persistenceEnabled = false; // Routing tests intentionally have no DB consumer.
    return cfg;
}

template <typename Fn>
bool WaitUntil(Fn&& fn, int timeoutMs = 1500)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (fn())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return fn();
}

Msg MakeConnData(uint16_t module, uint16_t method, uint64_t playerId)
{
    Msg m;
    m.head.msgType = MsgType::MSGTYPE_CONN_DATA;
    m.head.Module = module;
    m.head.Method = method;
    m.head.session = 1001;
    m.head.seq = 7;
    m.head.playerId = playerId;
    return m;
}
Map TestMap()
{
    Map map; std::string err;
    if(!Map::Load(std::string(SCENE_SOURCE_DIR)+"/config/map.yaml",50,map,err))
        throw std::runtime_error(err);
    return map;
}
Msg BindingMsg(uint64_t pid, uint16_t method, uint32_t epoch = 1)
{
    auto m=MakeConnData(GatewayPlayer::kModule,method,pid);
    m.head.clientBinding={pid,uint64_t(epoch),epoch,1,1};
    m.head.hasClientBinding=true;
    return m;
}
}  // namespace

TEST(LogicThreadLoadsPlayerBeforeEnterAndDrainsSaveAck)
{
    MsgBus bus(1); auto map=TestMap(); AoiConfig aoi;
    auto cfg=TestCfg(); cfg.persistenceEnabled=true; cfg.shutdownDrainMs=1000;
    SceneMetrics metrics(1);
    LogicThread lt(0,bus,cfg,aoi,map,metrics.worker(0)); CHECK(lt.start());
    CHECK(WaitUntil([&] { return metrics.worker(0).readRaw().lifecycle == WorkerLifecycle::Running; }));
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(42,GatewayPlayer::kOnline))));
    Msg request;
    CHECK(WaitUntil([&] { return bus.tryPopDbMySql(request); }));
    CHECK(metrics.worker(0).readRaw().handled >= 1);
    dbv::DbTask load; CHECK(dbv::DbTask::Decode(request.body,load));
    CHECK_EQ(load.kind,4);
    auto enter=MakeConnData(Module::PLAYER,Method::PLAYER_ENTER_SCENE,42);
    enter.head.hasClientBinding=true; enter.head.clientBinding={42,1,1,1,1};
    enter.body=gs::EnterSceneReq().SerializeAsString();
    CHECK(IsAccepted(bus.sendToWorker(0,enter)));
    dbv::DbResult loaded; loaded.playerId=42;
    loaded.data=dbv::Val::Arr({
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(100),dbv::Val::Int(50),dbv::Val::Int(5),
            dbv::Val::Int(10),dbv::Val::Int(20),dbv::Val::Int(0),dbv::Val::Int(1),dbv::Val::Int(7)})}),
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(64),dbv::Val::Int(0),dbv::Val::Int(7)})}),
        dbv::Val::Arr({})});
    Msg reply=request; reply.head.msgType=MSGTYPE_DB_RESULT_MYSQL; reply.body=loaded.Encode();
    CHECK(IsAccepted(bus.sendToWorker(0,reply)));
    Msg ack;
    CHECK(WaitUntil([&] { return bus.tryPopNet(ack); }));
    gs::EnterSceneAck sceneAck; CHECK(sceneAck.ParseFromString(ack.body));
    CHECK_EQ(sceneAck.code(),0); CHECK_EQ(sceneAck.x(),10); CHECK_EQ(sceneAck.y(),20);
    lt.requestDrain();
    // Loaded, unchanged player does not need a redundant write. ACK must drain.
    CHECK(WaitUntil([&] { return lt.drainFinished(); }));
    lt.stop(); CHECK(lt.drainSucceeded()); CHECK_EQ(lt.playerCount(),0u);
}

TEST(LogicThreadPeriodicSaveRunsUnderContinuousMessageLoad)
{
    MsgBus bus(1); auto map=TestMap(); AoiConfig aoi;
    auto cfg=TestCfg();
    cfg.persistenceEnabled=true;
    cfg.flushIntervalMs=20;
    cfg.shutdownDrainMs=500;
    cfg.luaDir=std::string(SCENE_SOURCE_DIR)+"/lua_script";
    SceneMetrics metrics(1);
    LogicThread lt(0,bus,cfg,aoi,map,metrics.worker(0)); CHECK(lt.start());

    const auto binding=BindingMsg(42,GatewayPlayer::kOnline);
    CHECK(IsAccepted(bus.sendToWorker(0,binding)));
    Msg load; CHECK(WaitUntil([&] { return bus.tryPopDbMySql(load); }));

    dbv::DbResult loaded; loaded.playerId=42;
    loaded.data=dbv::Val::Arr({
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(100),dbv::Val::Int(50),dbv::Val::Int(5),
            dbv::Val::Int(10),dbv::Val::Int(20),dbv::Val::Int(0),dbv::Val::Int(1),dbv::Val::Int(7)})}),
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(64),dbv::Val::Int(0),dbv::Val::Int(7)})}),
        dbv::Val::Arr({})});
    Msg loadedReply=load;
    loadedReply.head.msgType=MSGTYPE_DB_RESULT_MYSQL;
    loadedReply.body=loaded.Encode();
    CHECK(IsAccepted(bus.sendToWorker(0,loadedReply)));

    auto enter=MakeConnData(Module::PLAYER,Method::PLAYER_ENTER_SCENE,42);
    enter.head.hasClientBinding=true;
    enter.head.clientBinding=binding.head.clientBinding;
    enter.body=gs::EnterSceneReq().SerializeAsString();
    CHECK(IsAccepted(bus.sendToWorker(0,enter)));
    Msg ack; CHECK(WaitUntil([&] { return bus.tryPopNet(ack); }));

    gs::WalkReq walk;
    walk.set_sx(10); walk.set_sy(20); walk.set_dx(11); walk.set_dy(20);
    auto move=enter;
    move.head.Method=Method::PLAYER_MOVE;
    move.body=walk.SerializeAsString();
    CHECK(IsAccepted(bus.sendToWorker(0,move)));
    CHECK(WaitUntil([&] { return bus.tryPopNet(ack); }));

    Msg save;
    bool foundSave=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    uint64_t seq=100;
    while (std::chrono::steady_clock::now() < deadline && !foundSave)
    {
        // 持续制造非空 worker 队列，验证周期保存不依赖 onIdle()。
        auto noise=MakeConnData(9999,9999,4242);
        noise.head.seq=seq++;
        CHECK(IsAccepted(bus.sendToWorker(0,std::move(noise))));
        if (bus.tryPopDbMySql(save))
            foundSave=true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(foundSave);
    if (foundSave)
    {
        dbv::DbTask saved;
        CHECK(dbv::DbTask::Decode(save.body,saved));
        CHECK_EQ(saved.kind,5);

        dbv::DbResult success;
        success.playerId=42;
        success.saveVersion=saved.saveVersion;
        success.capturedRevision=saved.capturedRevision;
        success.saveLifecycleToken=saved.saveLifecycleToken;
        Msg completed=save;
        completed.head.msgType=MSGTYPE_DB_RESULT_MYSQL;
        completed.body=success.Encode();
        CHECK(IsAccepted(bus.sendToWorker(0,std::move(completed))));
    }

    lt.stop(); CHECK(lt.drainSucceeded());
}

TEST(LogicThreadDrainReportsUnresolvedLoadTimeout)
{
    MsgBus bus(1); auto map=TestMap(); AoiConfig aoi;
    auto cfg=TestCfg(); cfg.persistenceEnabled=true; cfg.shutdownDrainMs=30;
    SceneMetrics metrics(1);
    LogicThread lt(0,bus,cfg,aoi,map,metrics.worker(0)); CHECK(lt.start());
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(42,GatewayPlayer::kOnline))));
    Msg request; CHECK(WaitUntil([&] { return bus.tryPopDbMySql(request); }));
    lt.stop(); CHECK(lt.drainFinished()); CHECK(!lt.drainSucceeded());
    CHECK_EQ(lt.playerCount(),0u);
}

TEST(LogicThreadFinalSaveBarrierResumesDeferredLogin)
{
    MsgBus bus(1); auto map=TestMap(); AoiConfig aoi;
    auto cfg=TestCfg(); cfg.persistenceEnabled=true; cfg.shutdownDrainMs=500;
    cfg.luaDir=std::string(SCENE_SOURCE_DIR)+"/lua_script";
    SceneMetrics metrics(1);
    LogicThread lt(0,bus,cfg,aoi,map,metrics.worker(0)); CHECK(lt.start());
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(42,GatewayPlayer::kOnline))));
    Msg load; CHECK(WaitUntil([&] { return bus.tryPopDbMySql(load); }));
    dbv::DbResult loaded; loaded.playerId=42;
    loaded.data=dbv::Val::Arr({
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(100),dbv::Val::Int(50),dbv::Val::Int(5),
            dbv::Val::Int(10),dbv::Val::Int(20),dbv::Val::Int(0),dbv::Val::Int(1),dbv::Val::Int(7)})}),
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(64),dbv::Val::Int(0),dbv::Val::Int(7)})}),
        dbv::Val::Arr({})});
    Msg reply=load; reply.head.msgType=MSGTYPE_DB_RESULT_MYSQL; reply.body=loaded.Encode();
    CHECK(IsAccepted(bus.sendToWorker(0,reply)));
    auto enter=MakeConnData(Module::PLAYER,Method::PLAYER_ENTER_SCENE,42);
    enter.head.hasClientBinding=true; enter.head.clientBinding={42,1,1,1,1};
    enter.body=gs::EnterSceneReq().SerializeAsString();
    CHECK(IsAccepted(bus.sendToWorker(0,enter)));
    Msg ack; CHECK(WaitUntil([&] { return bus.tryPopNet(ack); }));
    gs::WalkReq walk; walk.set_sx(10); walk.set_sy(20); walk.set_dx(11); walk.set_dy(20);
    auto move=enter; move.head.Method=Method::PLAYER_MOVE; move.body=walk.SerializeAsString();
    CHECK(IsAccepted(bus.sendToWorker(0,move)));
    CHECK(WaitUntil([&] { return bus.tryPopNet(ack); }));
    gs::WalkAck walked; CHECK(walked.ParseFromString(ack.body)); CHECK_EQ(walked.x(),11);
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(42,GatewayPlayer::kOffline))));
    Msg save; CHECK(WaitUntil([&] { return bus.tryPopDbMySql(save); }));
    dbv::DbTask saved; CHECK(dbv::DbTask::Decode(save.body,saved)); CHECK_EQ(saved.kind,5);
    CHECK_EQ(saved.saveVersion,8u);
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(42,GatewayPlayer::kOnline,2))));
    CHECK(WaitUntil([&] { return lt.playerCount()==0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Msg next; CHECK(!bus.tryPopDbMySql(next));
    dbv::DbResult success; success.playerId=42; success.saveVersion=saved.saveVersion;
    success.capturedRevision=saved.capturedRevision; success.saveLifecycleToken=saved.saveLifecycleToken;
    Msg completed=save; completed.head.msgType=MSGTYPE_DB_RESULT_MYSQL; completed.body=success.Encode();
    CHECK(IsAccepted(bus.sendToWorker(0,completed)));
    CHECK(WaitUntil([&] { return bus.tryPopDbMySql(next); }));
    dbv::DbTask reloaded; CHECK(dbv::DbTask::Decode(next.body,reloaded)); CHECK_EQ(reloaded.kind,4);
    loaded.data.a[0].a[0].a[3]=dbv::Val::Int(11);
    loaded.data.a[0].a[0].a[7]=dbv::Val::Int(8);
    loaded.data.a[1].a[0].a[2]=dbv::Val::Int(8);
    Msg newReply=next; newReply.head.msgType=MSGTYPE_DB_RESULT_MYSQL; newReply.body=loaded.Encode();
    CHECK(IsAccepted(bus.sendToWorker(0,newReply)));
    auto reenter=enter; reenter.head.clientBinding={42,2,2,1,1};
    CHECK(IsAccepted(bus.sendToWorker(0,reenter)));
    CHECK(WaitUntil([&] { return bus.tryPopNet(ack); }));
    gs::EnterSceneAck ready; CHECK(ready.ParseFromString(ack.body)); CHECK_EQ(ready.x(),11);
    lt.stop(); CHECK(lt.drainSucceeded());
}

TEST(LogicThreadReloadRejectsOldResultsAndOfflineCancelsLoad)
{
    MsgBus bus(1); auto map=TestMap(); AoiConfig aoi;
    auto cfg=TestCfg(); cfg.persistenceEnabled=true; cfg.shutdownDrainMs=500;
    SceneMetrics metrics(1);
    LogicThread lt(0,bus,cfg,aoi,map,metrics.worker(0)); CHECK(lt.start());
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(42,GatewayPlayer::kOnline))));
    Msg load; CHECK(WaitUntil([&] { return bus.tryPopDbMySql(load); }));
    auto loadedResult=[] {
        dbv::DbResult r; r.playerId=42;
        r.data=dbv::Val::Arr({
            dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(100),dbv::Val::Int(50),dbv::Val::Int(5),
                dbv::Val::Int(10),dbv::Val::Int(20),dbv::Val::Int(0),dbv::Val::Int(1),dbv::Val::Int(7)})}),
            dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(64),dbv::Val::Int(0),dbv::Val::Int(7)})}),
            dbv::Val::Arr({})}); return r;
    };
    Msg reply=load; reply.head.msgType=MSGTYPE_DB_RESULT_MYSQL; reply.body=loadedResult().Encode();
    CHECK(IsAccepted(bus.sendToWorker(0,reply)));
    auto enter=MakeConnData(Module::PLAYER,Method::PLAYER_ENTER_SCENE,42);
    enter.head.hasClientBinding=true; enter.head.clientBinding={42,1,1,1,1};
    enter.body=gs::EnterSceneReq().SerializeAsString();
    CHECK(IsAccepted(bus.sendToWorker(0,enter)));
    Msg ack; CHECK(WaitUntil([&] { return bus.tryPopNet(ack); }));
    // An unchanged loaded character needs no final write, but a re-login must
    // still load under a new request identity and reject the old result.
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(42,GatewayPlayer::kOffline))));
    CHECK(WaitUntil([&] { return lt.playerCount()==0; }));
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(42,GatewayPlayer::kOnline,2))));
    Msg nextLoad; CHECK(WaitUntil([&] { return bus.tryPopDbMySql(nextLoad); }));
    CHECK(nextLoad.head.taskId!=load.head.taskId);
    // A stale result cannot complete the new binding's load.
    CHECK(IsAccepted(bus.sendToWorker(0,reply)));
    auto second=enter; second.head.clientBinding={42,2,2,1,1};
    CHECK(IsAccepted(bus.sendToWorker(0,second)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!bus.tryPopNet(ack));
    auto offline=BindingMsg(42,GatewayPlayer::kOffline,2);
    CHECK(IsAccepted(bus.sendToWorker(0,offline)));
    CHECK(WaitUntil([&] { return lt.playerCount()==0; }));
    Msg fresh=nextLoad; fresh.head.msgType=MSGTYPE_DB_RESULT_MYSQL; fresh.body=loadedResult().Encode();
    CHECK(IsAccepted(bus.sendToWorker(0,fresh)));
    lt.stop(); CHECK(lt.drainSucceeded());
}

// 1.16 的回归点：无效路由（Module/Method 不在路由表）**不得**创建 Actor。
// 修前 onNetMsg 先 ensureActor 再判路由 → 这类垃圾消息会凭空造出 Actor（players=1）。
TEST(LogicThreadInvalidRouteDoesNotCreateActor)
{
    MsgBus bus(1);
    auto map=TestMap(); AoiConfig aoi;
    SceneMetrics metrics(1);
    LogicThread lt(0, bus, TestCfg(),aoi,map,metrics.worker(0));
    CHECK(lt.start());

    CHECK(IsAccepted(bus.sendToWorker(0, MakeConnData(9999, 9999, 4242))));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(lt.playerCount(), (size_t)0);

    // 换一个 playerId 再来一次：仍然不创建
    CHECK(IsAccepted(bus.sendToWorker(0, MakeConnData(8888, 8888, 5555))));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(lt.playerCount(), (size_t)0);

    lt.stop();
}

// 错误体沿用 gs.RetTip，但回程头必须匹配原请求，否则网关拒绝并断开后端。
TEST(LogicThreadErrorResponseKeepsRequestContext)
{
    MsgBus bus(1);
    auto map=TestMap(); AoiConfig aoi;
    SceneMetrics metrics(1);
    LogicThread lt(0, bus, TestCfg(),aoi,map,metrics.worker(0));
    CHECK(lt.start());

    for (const auto& request : {MakeConnData(9999, 9999, 4242),
                                MakeConnData(Module::PLAYER, Method::PLAYER_MOVE, 4242)})
    {
        CHECK(IsAccepted(bus.sendToWorker(0, request)));
        Msg response;
        CHECK(WaitUntil([&] { return bus.tryPopNet(response); }));
        CHECK_EQ(response.head.Module, request.head.Module);
        CHECK_EQ(response.head.Method, request.head.Method);
        CHECK_EQ(response.head.seq, request.head.seq);
        CHECK_EQ(response.head.playerId, request.head.playerId);
        CHECK_EQ(response.head.session, request.head.session);
        gs::RetTip tip;
        CHECK(tip.ParseFromString(response.body));
        if (request.head.Module == 9999)
            CHECK_EQ(tip.code(), 1001);
        else
            CHECK(tip.code() != 0);  // Lua 环境故障的具体错误码取决于初始化结果
    }
    lt.stop();
}

// 正向对照：合法玩法请求不创建 Actor，可信 ONLINE 才建立绑定。
TEST(LogicThreadValidRouteRequiresOnlineBinding)
{
    MsgBus bus(1);
    auto map=TestMap(); AoiConfig aoi;
    SceneMetrics metrics(1);
    LogicThread lt(0, bus, TestCfg(),aoi,map,metrics.worker(0));
    CHECK(lt.start());

    CHECK(IsAccepted(bus.sendToWorker(0, MakeConnData(Module::PLAYER, Method::PLAYER_MOVE, 4242))));
    CHECK(WaitUntil([&] { return metrics.worker(0).readRaw().handled >= 1; }));
    CHECK_EQ(lt.playerCount(),0u);
    CHECK(IsAccepted(bus.sendToWorker(0,BindingMsg(4242,GatewayPlayer::kOnline))));
    CHECK(WaitUntil([&] { return lt.playerCount() == 1; }));

    lt.stop();
}

// 内部 ConnClose 不删除玩家；完整绑定的 OFFLINE 精确清理。
TEST(LogicThreadClientOfflineIsBindingScoped)
{
    MsgBus bus(1);
    auto map=TestMap(); AoiConfig aoi;
    SceneMetrics metrics(1);
    LogicThread lt(0, bus, TestCfg(),aoi,map,metrics.worker(0));
    CHECK(lt.start());

    CHECK(IsAccepted(bus.sendToWorker(0, BindingMsg(4242,GatewayPlayer::kOnline))));
    CHECK(WaitUntil([&] { return lt.playerCount() == 1; }));

    Msg closeMsg;
    closeMsg.head.msgType = MsgType::MSGTYPE_CONN_CLOSE;
    closeMsg.head.session = 1001;
    closeMsg.head.playerId = 4242;
    CHECK(IsAccepted(bus.sendToWorker(0, std::move(closeMsg))));
    CHECK(IsAccepted(bus.sendToWorker(0, BindingMsg(4242,GatewayPlayer::kOnline,2))));
    CHECK(IsAccepted(bus.sendToWorker(0, BindingMsg(4242,GatewayPlayer::kOffline,1))));
    CHECK(WaitUntil([&] { return metrics.worker(0).readRaw().handled >= 4; }));
    CHECK_EQ(lt.playerCount(),1u);
    CHECK(IsAccepted(bus.sendToWorker(0, BindingMsg(4242,GatewayPlayer::kOffline,2))));
    CHECK(WaitUntil([&] { return lt.playerCount() == 0; }));
    CHECK(IsAccepted(bus.sendToWorker(0, BindingMsg(4242,GatewayPlayer::kOnline,2))));
    CHECK(WaitUntil([&] { return metrics.worker(0).readRaw().handled >= 6; }));
    CHECK_EQ(lt.playerCount(),0u);

    lt.stop();
}

// 无效路由的垃圾消息也不该刷新"活跃时间"（否则攻击者可用垃圾消息让空闲回收永不生效）。
// 这里用"创建/不创建"间接覆盖：Actor 都不存在，自然也就不存在被刷新的活跃度。
TEST(LogicThreadInvalidRouteKeepsNoState)
{
    MsgBus bus(1);
    auto map=TestMap(); AoiConfig aoi;
    SceneMetrics metrics(1);
    LogicThread lt(0, bus, TestCfg(),aoi,map,metrics.worker(0));
    CHECK(lt.start());

    for (uint64_t pid = 10000; pid < 10005; ++pid)
        CHECK(IsAccepted(bus.sendToWorker(0, MakeConnData(7777, 7777, pid))));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(lt.playerCount(), (size_t)0);

    lt.stop();
}
