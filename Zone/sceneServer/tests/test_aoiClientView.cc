#include "test_util.h"
#include "service/aoi/aoiClientView.h"
#include "common/msg.h"
#include "player/player.h"

namespace
{
struct Sink final : AoiPushSink
{
    std::vector<AoiClientPushRequest> pushes;
    AoiPushResult result=AoiPushResult::kAccepted;
    AoiPushResult sendToClient(const AoiClientPushRequest& request) override
    {
        pushes.push_back(request);
        return result;
    }
    AoiWorkerPublishResult publishToWorkers(const AoiWorkerPublishRequest&) override { return {}; }
};

Map LoadMapForView()
{
    Map map;
    std::string error;
    CHECK(Map::Load(std::string(SCENE_SOURCE_DIR) + "/config/map.yaml", 50, map, error));
    return map;
}
}

TEST(AoiClientViewFirstSyncUsesObserverBusinessId)
{
    Sink sink;
    AoiConfig cfg;
    Map map = LoadMapForView();
    AoiServiceHooks hooks;
    hooks.processObserver = [](AoiService& service, EntityId observer) {
        return ProcessAoiClientObserver(service, observer);
    };
    AoiService service(0, cfg, sink, map, hooks);

    Player player;
    player.set_id(1001);
    player.setPos(10, 10, 0);
    CHECK_EQ(service.OnEntityEnter(player), AoiService::ApplyResult::kApplied);
    CHECK(service.OnPlayerOnline(1001, 1, 7));
    service.OnTick(0);

    CHECK_EQ(sink.pushes.size(), 1u);
    CHECK_EQ(sink.pushes.front().observerPlayerId, 1001u);
    CHECK_EQ(sink.pushes.front().clientViewEpoch, 7u);
    CHECK_EQ(sink.pushes.front().method, Method::AOI_VIEW_SNAPSHOT);
}

TEST(AoiClientViewBorrowPreservesFailedSendAndSessionReset)
{
    Sink sink;
    AoiConfig cfg;
    Map map=LoadMapForView();
    AoiService service(0,cfg,sink,map,{});
    Player player; player.set_id(1001);player.setPos(10,10,0);
    CHECK_EQ(service.OnEntityEnter(player),AoiService::ApplyResult::kApplied);
    Player target;target.set_id(1002);target.setPos(12,10,0);
    CHECK_EQ(service.OnEntityEnter(target),AoiService::ApplyResult::kApplied);
    CHECK(service.OnPlayerOnline(1001,1,7));
    const auto observer=service.EntityOf(1001);
    CHECK(observer.has_value());
    const auto targetId=service.EntityOf(1002);
    CHECK(targetId.has_value());
    CHECK(ProcessAoiClientObserver(service,*observer));
    const auto* baseline=service.views().borrowCommittedBaseline(*observer);
    CHECK(baseline != nullptr);
    CHECK(baseline->contains(*targetId));
    const auto initial=baseline->at(*targetId).spatialVersion;
    const auto sequence=service.views().prepareSend(*observer)->nextViewSeq;
    target.setPos(13,10,0);
    CHECK_EQ(service.OnLocalEntityMoved(target),AoiService::ApplyResult::kApplied);
    sink.result=AoiPushResult::kQueueFull;
    CHECK(!ProcessAoiClientObserver(service,*observer));
    CHECK_EQ(service.views().borrowCommittedBaseline(*observer)->at(*targetId).spatialVersion,initial);
    CHECK_EQ(service.views().prepareSend(*observer)->nextViewSeq,sequence);
    sink.result=AoiPushResult::kAccepted;
    CHECK(ProcessAoiClientObserver(service,*observer));
    CHECK(service.views().borrowCommittedBaseline(*observer)->at(*targetId).spatialVersion>initial);
    CHECK_EQ(service.views().prepareSend(*observer)->nextViewSeq,sequence+1);
    const auto sent=sink.pushes.size();
    CHECK(ProcessAoiClientObserver(service,*observer));
    CHECK_EQ(sink.pushes.size(),sent);
    CHECK(service.views().resetObserver(*observer,7,8));
    CHECK(service.views().borrowCommittedBaseline(*observer)->empty());
    CHECK(ProcessAoiClientObserver(service,*observer));
    CHECK_EQ(sink.pushes.back().clientViewEpoch,8u);
    CHECK_EQ(sink.pushes.back().method,Method::AOI_VIEW_SNAPSHOT);
}