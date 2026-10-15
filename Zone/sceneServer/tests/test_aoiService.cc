#include "test_util.h"
#include "service/aoi/aoiService.h"
#include "player/player.h"
#include <stdexcept>

namespace {
using Result = AoiService::ApplyResult;
struct FakeSink : AoiPushSink
{
    bool rejectTwo = true;
    std::vector<std::vector<workerId>> attempts;
    AoiPushResult sendToClient(const AoiClientPushRequest&) override { return AoiPushResult::kAccepted; }
    AoiWorkerPublishResult publishToWorkers(const AoiWorkerPublishRequest& request) override
    {
        attempts.push_back(request.targets);
        AoiWorkerPublishResult result;
        for(auto target : request.targets) result.deliveries.push_back({target, !(rejectTwo && target == 2)});
        return result;
    }
};
Map LoadMap()
{
    Map map;
    std::string err;
    if(!Map::Load(std::string(SCENE_SOURCE_DIR) + "/config/map.yaml", 50, map, err))
        throw std::runtime_error(err);
    return map;
}
AoiEvent Remote(AoiMsgKind kind, uint64_t version, uint64_t seq)
{
    AoiEvent ev;
    ev.msgKind = kind;
    ev.eId = uint64_t(1) << 32;
    ev.businessId = 900;
    ev.entityKind = EntityKind::kPlayer;
    ev.entityGeneration = 1;
    ev.spatialVersion = version;
    ev.ownerSeq = seq;
    ev.ownerThreadId = 1;
    ev.target_x = 20;
    ev.target_y = 20;
    return ev;
}
}

TEST(AoiServiceLocalLifecycleAndPublishRetry)
{
    auto map = LoadMap();
    AoiConfig cfg;
    FakeSink sink;
    AoiServiceHooks hooks;
    hooks.publishTargets = {0, 1, 2, 2};
    hooks.encodeEvent = [](const AoiEvent& ev, AoiWorkerPublishRequest& request) {
        request.msgType = 1; // 测试适配器，不声明真实协议号。
        request.body = std::to_string(ev.ownerSeq);
        return true;
    };
    AoiService service(0, cfg, sink, map, hooks);
    Player p;
    p.set_id(100);
    p.setPos(10, 10, 0);
    CHECK_EQ(service.OnEntityEnter(p), Result::kApplied);
    const auto id = *service.EntityOf(100);
    CHECK_EQ(service.OnEntityEnter(p), Result::kIgnored);
    CHECK_EQ(service.BuildOwnerSnapshot().seq, 1u);
    service.OnTick(0);
    CHECK_EQ(sink.attempts.size(), 1u);
    CHECK_EQ(sink.attempts[0], (std::vector<workerId>{1, 2}));
    CHECK_EQ(service.Stats().pendingPublishes, 1u);
    service.OnTick(1);
    CHECK_EQ(service.Stats().ticks, 1u);
    sink.rejectTwo = false;
    service.OnTick(50);
    CHECK_EQ(sink.attempts[1], (std::vector<workerId>{2}));
    CHECK_EQ(service.Stats().pendingPublishes, 0u);
    p.setPos(10, 10, 1);
    CHECK_EQ(service.OnLocalEntityMoved(p), Result::kApplied);
    CHECK_EQ(service.BuildOwnerSnapshot().ents[0].spatialVersion, 2u);
    CHECK(service.OnPlayerOnline(100, 8, 10));
    CHECK(service.OnPlayerRebind(100, 10, 11));
    CHECK(!service.OnPlayerOffline(100, 10, 0));
    CHECK(service.OnPlayerOffline(100, 11, 0));
    CHECK_EQ(service.OnEntityLeave(id), Result::kApplied);
    CHECK(service.BuildOwnerSnapshot().ents.empty());
    CHECK_EQ(service.OnEntityEnter(p), Result::kApplied);
    CHECK_EQ(*service.EntityOf(100), id);
    CHECK_EQ(service.BuildOwnerSnapshot().ents[0].entityGeneration, 2u);
    p.setPos(-1, 10, 0);
    CHECK_EQ(service.OnLocalEntityMoved(p), Result::kOutOfBounds);
}

TEST(AoiServiceRemoteCoalescingAndSnapshotRepair)
{
    auto map = LoadMap();
    AoiConfig cfg;
    FakeSink sink;
    AoiService service(0, cfg, sink, map);
    Player p;
    p.set_id(100);
    p.setPos(10, 10, 0);
    service.OnEntityEnter(p);
    const auto id = *service.EntityOf(100);
    auto entry = Remote(AoiMsgKind::kEntry, 1, 1);
    CHECK_EQ(service.onRemoteEntityEvent(2, entry), AoiService::RemoteResult::kRejected);
    CHECK_EQ(service.onRemoteEntityEvent(1, entry), AoiService::RemoteResult::kApplied);
    auto move = Remote(AoiMsgKind::kMove, 3, 3);
    move.target_x = 30;
    CHECK_EQ(service.onRemoteEntityEvent(1, move), AoiService::RemoteResult::kQueued);
    CHECK_EQ(service.onRemoteEntityEvent(1, Remote(AoiMsgKind::kMove, 2, 2)), AoiService::RemoteResult::kIgnored);
    std::vector<AoiEntity> visible;
    CHECK(service.ViewOf(id, visible));
    CHECK_EQ(visible[0].spatialVersion, 1u);
    service.OnTick(0);
    CHECK(service.ViewOf(id, visible));
    CHECK_EQ(visible.size(), 1u);
    CHECK_EQ(visible[0].spatialVersion, 3u);
    move.eId += 1;
    move.businessId += 1;
    service.onRemoteEntityEvent(1, move);
    service.OnTick(50);
    CHECK_EQ(service.OwnersNeedingSnapshot(), (std::vector<workerId>{1}));
    CHECK_EQ(service.ApplyOwnerSnapshot(1, AoiOwnerSnapshot{4, 1, {}}), Result::kApplied);
    CHECK(service.OwnersNeedingSnapshot().empty());
    CHECK(service.ViewOf(id, visible));
    CHECK(visible.empty());
}

TEST(AoiServiceUnboundHooksPreserveWork)
{
    auto map = LoadMap();
    AoiConfig cfg;
    FakeSink sink;
    AoiServiceHooks hooks;
    hooks.publishTargets = {1};
    AoiService service(0, cfg, sink, map, hooks);
    Player p;
    p.set_id(100);
    p.setPos(10, 10, 0);
    service.OnEntityEnter(p);
    service.OnPlayerOnline(100, 1, 1);
    service.OnTick(0);
    CHECK_EQ(service.Stats().pendingPublishes, 1u);
    CHECK(service.Stats().dirtyObservers != 0);
    CHECK(sink.attempts.empty());
    CHECK(service.views().needResync(*service.EntityOf(100)));
}