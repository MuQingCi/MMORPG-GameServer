#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOISERVICE_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOISERVICE_H

#include "config/aoiConfig.h"
#include "service/aoi/aoiPushSink.h"
#include "service/aoi/aoiReplica.h"
#include "service/aoi/aoiViewSet.h"
#include <functional>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class AoiService;

struct AoiServiceStats
{
    size_t pendingRemoteMoves = 0;
    size_t pendingPublishes = 0;
    size_t dirtyObservers = 0;
    size_t observers = 0;
    size_t ownersNeedingSnapshot = 0;
    uint64_t ticks = 0;
};

// 未绑定协议适配器时保留工作，不把尚未发送的内容当作成功。
struct AoiServiceHooks
{
    // 多 worker 部署必须填写其余全部 worker，默认空集合仅代表单 worker 模式。
    std::vector<workerId> publishTargets;
    std::function<bool(const AoiEvent&, AoiWorkerPublishRequest&)> encodeEvent;
    // 不得重入生命周期、OnTick 或修改发布队列；仅同步处理当前观察者推送。
    // true 表示本轮已处理；pending/resync 未消除仍会在下一 tick 被选中。
    std::function<bool(AoiService&, EntityId)> processObserver;
};

class AoiService
{
public:
    using ApplyResult = AoiReplica::ApplyResult;
    enum class RemoteResult { kQueued, kApplied, kIgnored, kRejected };
    AoiService(workerId selfWorkerId, const AoiConfig& cfg, AoiPushSink& sink,
               const Map& map, AoiServiceHooks hooks = {});
    AoiService(const AoiService&) = delete;
    AoiService& operator=(const AoiService&) = delete;
    AoiService(AoiService&&) = delete;
    AoiService& operator=(AoiService&&) = delete;

    ApplyResult OnEntityEnter(Player& p);
    ApplyResult OnEntityReenter(Player& p);
    ApplyResult OnEntityLeave(EntityId id);
    ApplyResult OnLocalEntityMoved(Player& p);

    // sender 来自可信消息信封；MOVE 只暂存，到 tick 才应用。
    RemoteResult onRemoteEntityEvent(workerId sender, const AoiEvent& ev);
    
    ApplyResult ApplyOwnerSnapshot(workerId sender, const AoiOwnerSnapshot& snap);
    
    AoiOwnerSnapshot BuildOwnerSnapshot() const;

    //推送
    AoiPushResult pushToClient(const AoiClientPushRequest& request);
    AoiWorkerPublishResult pushToWorkers(const AoiWorkerPublishRequest& request);

    //玩家事件
    bool OnPlayerOnline(uint64_t pid, uint64_t gatewayId, uint32_t clientViewEpoch);
    bool OnPlayerRebind(uint64_t pid, uint32_t oldEpoch, uint32_t newEpoch);
    bool OnPlayerOffline(uint64_t pid, uint32_t clientViewEpoch, uint8_t reason);
    
    bool OnViewResyncReq(uint64_t pid, uint32_t clientViewEpoch);
    bool OnClientSendFailed(EntityId observer, uint32_t clientViewEpoch);

    void OnTick(int64_t now);
    AoiViewSet& views();
    
    bool ViewOf(EntityId observer, std::vector<AoiEntity>& out) const;
    AoiServiceStats Stats() const;

    std::optional<EntityId> EntityOf(uint64_t playerId) const;

    std::vector<workerId> OwnersNeedingSnapshot() const;

private:
    struct LocalIdentity { EntityId id = 0; uint32_t generation = 0; };
    struct PublishTask { AoiEvent event; std::vector<workerId> remaining; };
    
    ApplyResult Enter(Player& p, bool reenter);
    ApplyResult ApplyLocal(const AoiEvent& ev);
    void FlushRemoteMoves();
    void RetryPublishes();
    void ProcessObservers();
    bool ValidPosition(const Player& p) const;

    workerId wId_;
    AoiConfig aoiConfig_;
    AoiPushSink& sink_;
    const Map& map_;
    AoiServiceHooks hooks_;
    AoiViewSet viewSet_;
    AoiReplica replica_;

    // ID = worker 高32位 + 本地计数低32位；仅保证同场景进程运行期唯一。
    std::unordered_map<uint64_t, LocalIdentity> identities_;
    
    std::unordered_set<EntityId> dirtyObservers_;
    std::unordered_map<EntityId, AoiEvent> pendingRemoteMoves_;
    std::unordered_set<workerId> ownersNeedingSnapshot_;
    std::vector<PublishTask> pendingPublishes_;
    uint32_t nextEntityCounter_ = 1;
    uint64_t ownerSeq_ = 0;
    int64_t nextTickTime_ = 0;
    uint64_t tickCount_ = 0;
};
#endif