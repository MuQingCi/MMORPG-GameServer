#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIVIEWSET_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIVIEWSET_H

#include "aoiTypes.h"

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

struct clientViewKey
{
    uint32_t gatewayId = 0;
};

struct SentEntityState
 {
    EntityKind kind       = EntityKind::kPlayer;    //实例类型
    uint64_t   businessId = 0;        // 玩家 = playerId；怪物/NPC = 配置 id
    
    uint32_t entityGeneration = 0;    // 实体"进入场景"的世代：每次 ENTER 递增；LEAVE 后作废
    uint64_t spatialVersion   = 0;    // 仅位置/朝向的空间版本
};

struct ObserverSnapshot
{
    //实例已提交状态
    std::unordered_map<EntityId, SentEntityState> committedState_;

    //待处理实体ID集合
    std::unordered_set<EntityId> pendings_;

    uint32_t clientViewEpoch = 0;
    uint64_t nextViewSeq = 1;

    bool needResync = false;
    uint32_t resyncRevision = 0;
};


struct ViewSendToken
{
    EntityId observerId  = 0;
    uint32_t clientViewEpoch = 0;
    uint64_t nextViewSeq = 1;
};

struct AcceptedPush
{
    AoiMsgKind kind = AoiMsgKind::kInvalid;
    EntityId targetId = 0;
    SentEntityState state;
};

// 一个替换快照是一个逻辑序号；所有分片携带相同 snapshotId/viewSeq。
struct ViewSnapshotToken
{
    EntityId observerId = 0;
    uint32_t clientViewEpoch = 0;
    uint64_t viewSeq = 0;
    uint32_t resyncRevision = 0;
    uint64_t snapshotId = 0;
    uint32_t partCount = 0;
};

using ViewBaseline = std::unordered_map<EntityId, SentEntityState>;

/**
 * @brief 当前类只保存本线程实例ID对应的已提交的AOI快照——推送已成功入队的实体状态基线
 * 
 */

class AoiViewSet
{
public:
    AoiViewSet();
    ~AoiViewSet();
    
    bool registryObserver(EntityId observerId, uint32_t clientViewEpoch);
    bool resetObserver(EntityId observerId, uint32_t expectedOldViewEpoch, uint32_t newViewEpoch);
    bool removeObserver(EntityId observerId, uint32_t expectedViewEpoch);

    //获取当前会话键和候选序号，不推进基线
    std::optional<ViewSendToken> prepareSend(EntityId eid) const;
    
    //查询,不存在则返回nullptr
    const ObserverSnapshot* findObserver(EntityId observerId) const;
    const SentEntityState* findSentState(EntityId observerId, EntityId targetId) const;

    // 一次调用对应一个已成功入队的帧，items 按帧内顺序处理。
    // 整帧校验成功才提交基线、清理对应 pending 并消耗一个序号；空帧不消耗。
    // false 表示本地提交不成立，不是入队失败；上层需判断是否安排重同步。
    // Entry 可替换同身份的新世代；Move 只允许同世代；旧世代 Leave 幂等忽略。
    bool commitAccepted(EntityId observerId, 
                        const ViewSendToken& token,
                        const std::vector<AcceptedPush>& items);

    bool markPending(EntityId observerId, EntityId targetId, const ViewSendToken& token);
    //仅在目标已成功提交，或重新计算后确认不需要任何推送时清理
    bool clearResolvedPending(EntityId observerId, EntityId targetId);
    
    bool hasPending(EntityId observerId) const;
    bool needResync(EntityId observerId) const;

    const std::vector<EntityId> forEachObserver() const;
    bool markNeedResync(EntityId observerId, uint32_t clientEpoch);

    // frozenBaseline 必须来自此次实际发送的固定视野，不得在完成时重新查询 Replica。
    // partCount 包含完整替换所需的全部分片；空视野仍至少发送一个空快照分片。
    std::optional<ViewSnapshotToken> beginSnapshot(EntityId observerId,
                                                   const ViewBaseline& frozenBaseline,
                                                   uint32_t partCount);
    // 仅在对应分片 sendToNet 返回 true 后调用，重复确认幂等。
    bool markSnapshotPartAccepted(const ViewSnapshotToken& token, uint32_t partIndex);
    // 全部分片入队后才替换基线、清 pending、消耗序号并清恢复标记。
    bool commitSnapshot(const ViewSnapshotToken& token);
    // 取消任务不会清恢复标记或推进发送基线。
    bool abortSnapshot(const ViewSnapshotToken& token);

    // 回收还必须满足 Replica 的 owner 安全水位条件。
    // pending 只有 ID，没有世代，故对任意世代均保守视为引用。
    bool references(EntityId targetId) const;
    bool references(EntityId targetId, uint32_t entityGeneration) const;
private:
    struct SnapshotTask
    {
        ViewSnapshotToken token;
        ViewBaseline baseline;
        std::vector<bool> acceptedParts;
        uint32_t acceptedCount = 0;
    };
    SnapshotTask* findSnapshotTask(const ViewSnapshotToken& token);
    bool processEntry(const AcceptedPush& push, ObserverSnapshot& obss);
    bool processMove(const AcceptedPush& push, ObserverSnapshot& obss);
    bool processLeave(const AcceptedPush& push, ObserverSnapshot& obss);
    

    //观察者实例ID->对应快照
    std::unordered_map<EntityId, ObserverSnapshot> views_;

    std::unordered_map<EntityId, SnapshotTask> snapshotTasks_;
    // 不随观察者删除/重建复位，防止旧任务 token 命中新任务。
    uint64_t nextSnapshotId_ = 1;

    std::atomic<uint64_t> failedSendNum_ = {0};//发送失败计数器
};

#endif