#include "test_util.h"
#include "service/aoi/aoiClientView.h"
#include "common/msg.h"
#include "player/player.h"

namespace
{
struct Sink final : AoiPushSink
{
    std::vector<AoiClientPushRequest> pushes;
    AoiPushResult sendToClient(const AoiClientPushRequest& request) override
    {
        pushes.push_back(request);
        return AoiPushResult::kAccepted;
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