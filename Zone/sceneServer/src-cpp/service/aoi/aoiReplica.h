#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIREPLICA_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIREPLICA_H

#include "service/aoi/aoiGrid.h"
#include "service/aoi/aoiTypes.h"
#include "config/aoiConfig.h"

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>


/**
 * @brief 只负责保存副本、应用外部事件并返回应用结果；内部应用事件需校验版本，删除实例需创建墓碑
 * 
 */
class AoiReplica
{
public:
    enum class Source { kLocal, kRemote };
    enum class ApplyResult { kIgnored, kApplied, kNewGeneration, kBadSource, kOutOfBounds, kInvalid, kNeedSnapshot };

    AoiReplica(workerId selfWorkerId, const AoiConfig& config, const Map& map);
    ~AoiReplica();

    ApplyResult Apply(const AoiEvent& ev, Source src, std::unordered_set<uint64_t>& dirtyObservers);

    void addWatcher(uint64_t observerId, uint32_t cell);
    void removeWatcher(uint64_t observerId, uint32_t cell);

    const AoiCell& cell(uint32_t cellIdx) const;
    const AoiEntity* findById(uint64_t id) const;

    void ForEachOwned(const std::function<void(const AoiEntity&)>& fn) const;
    // 只接受远端 owner 的完整快照；成功后才推进覆盖水位。
    ApplyResult ApplyOwnerSnapshot(const AoiOwnerSnapshot& snap, std::unordered_set<uint64_t>& dirtyObservers);
private:
    //墓碑
    struct Tombstone
    {
        uint32_t generation = 0;     // 离开时的实体世代
        uint64_t version    = 0;     // 离开时的 spatialVersion（已 ++）
        uint64_t ownerSeq   = 0;     // 离开事件在 owner 流中的序号（回收水位用）
        uint32_t ownerWorker = 0;
        int64_t  atMs       = 0;     // 仅用于内存保护 TTL
    };

    //两种处理流程
    void processLocalAoiMsg(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers);
    void processRemoteAoiMsg(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers);

    //本地事件
    void onLocalEntry(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers);
    void onLocalMove(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers);
    void onLocalLeave(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers);

    //远端事件
    void onRemoteEntry(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers);
    void onRemoteMove(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers);
    void onRemoteLeave(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers);

    void onInvalid(const AoiEvent& ev, uint32_t cellIdx);

    //辅助函数
    void initTombstone(Tombstone& tomb, const AoiEvent& ev);

    void addLocalEntity(AoiEntity& ent, uint32_t cellIdx);
    void removeLocalEntity(AoiEntity& ent, uint32_t cellIdx);
    
    void addRemoteEntiy(AoiEntity& ent, uint32_t cellIdx);
    void removeRemoteEntity(AoiEntity& ent, uint32_t cellIdx);

    workerId wId_;
    const AoiConfig& config_;
    const Map& map_;

    std::unique_ptr<AoiGrid> grid_;
    std::unordered_map<EntityId, AoiEntity> ents_;     // ★ 内部唯一全场景实例轻量状态权威副本

    std::unordered_map<EntityId, Tombstone> tombstones_;
    // 每个 owner 已完成的快照水位：snapshotSeq 单调，用于安全回收 tombstone（P1-2）
    //OwnerId->ApplySeq
    std::unordered_map<uint32_t, uint64_t> ownerSnapshotSeq_;
};

#endif