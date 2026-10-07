#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOISERVICE_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOISERVICE_H

#include "common/msgBus.h"
#include "player/player.h"
#include "config/aoiConfig.h"
#include "aoiViewSet.h"
#include "aoiReplica.h"
#include "service/aoi/aoiTypes.h"

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

class AoiService
{
public:
    AoiService(workerId selfWorkerId, const AoiConfig& cfg, AoiPushSink* sink,const Map& map);

    // ---- 生命周期----
    void OnEntityEnter(Player& p);        // 分配新 entityGeneration，发布 ENTER
    void OnEntityReenter(Player& p);      // 递增 entityGeneration，发布新世代 ENTER
    void OnEntityLeave(EntityId id);      // 作废世代、spatialVersion++、发布 LEAVE、写 tombstone
    void OnLocalEntityMoved(Player& p);   // 最小收口：判越界→递增版本→Apply(kLocal)→作为观察者更新覆盖格

    // ---- 内部事件----
    void ApplyImmediately(const AoiEvent& ev);      // ENTER / LEAVE
    void CoalesceBatch(const AoiBatchEvent& ev);    // AOI_MOVE_BATCH：每实体保留最大 stamp
    void ApplyOwnerSnapshot(const AoiOwnerSnapshot& ev);  // 分片重组 + 水位 + 缺失回收

    // ---- 会话生命周期（§7.6.1、D4.5；来自网关，非网络业务帧）----
    void OnPlayerOnline(uint64_t pid, uint64_t gatewayId, uint32_t clientViewEpoch);
    void OnPlayerRebind(uint64_t pid, uint32_t oldEpoch, uint32_t newEpoch);
    void OnPlayerOffline(uint64_t pid, uint32_t clientViewEpoch, uint8_t reason);
    void OnOnlineSetSnapshot(const std::vector<SessionEntry>& online);   // 租约/在线集合对齐（§8.4.2）

    // ---- 客户端重同步请求（§7.6.3；C2S，经 §11.2 的 C++ 分支进入）----
    void OnViewResyncReq(uint64_t observerBusinessId, const std::string& body);

    // ---- tick 与丢帧反馈 ----
    void OnTick(int64_t now);   // §8.1 顺序契约 + 四道预算闸
    AoiViewSet& views();      // onAoiSendFailed 用：MarkNeedResync(observerId)

    // ---- 只读查询与统计 ----
    bool ViewOf(EntityId observer, std::vector<AoiEntity>& out) const;  // scene.view
    AoiStats Snapshot() const;

private:
    workerId wId_;
    const AoiConfig& aoiConfig_;
    MsgBus* msgBus_;
    

    std::unique_ptr<AoiViewSet> viewSet_;
    std::unique_ptr<AoiReplica> replica_;
    
    std::unordered_set<uint64_t> dirtyObservers_;
    std::unordered_map<uint64_t, AoiEvent> pendingEvent_;      //待处理事件：合并Move
};



#endif