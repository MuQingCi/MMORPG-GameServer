#include "service/aoi/aoiViewSet.h"
#include "service/aoi/aoiTypes.h"
#include <cstddef>
#include <utility>
#include <vector>
#include <limits>

namespace 
{
void initObserverSnapshot(ObserverSnapshot& obSnapshot, uint32_t newViewEpoch)
{
    obSnapshot.committedState_.clear();
    obSnapshot.pendings_.clear();
    obSnapshot.clientViewEpoch  = newViewEpoch;
    obSnapshot.nextViewSeq = 1;
    obSnapshot.needResync = true;
    obSnapshot.resyncRevision = 0;
}

void initObserverState(const ObserverSnapshot& obSnapshot, ViewSendToken& obState, EntityId eid)
{
    obState.observerId      = eid;
    obState.nextViewSeq     = obSnapshot.nextViewSeq;
    obState.clientViewEpoch = obSnapshot.clientViewEpoch;
}
}


AoiViewSet::AoiViewSet()
{

}

AoiViewSet::~AoiViewSet()
{

}


bool AoiViewSet::registryObserver(EntityId observerId, uint32_t clientViewEpoch)
{
    //确保该观察者无视野/视野世代小于添加世代
    auto vIt = views_.find(observerId);
    if(vIt != views_.end() && vIt->second.clientViewEpoch >= clientViewEpoch)
        return false;

    ObserverSnapshot obSnapshot;
    obSnapshot.clientViewEpoch = clientViewEpoch;   
    obSnapshot.needResync = true; 
    views_[observerId] = std::move(obSnapshot);
    snapshotTasks_.erase(observerId);
    return true;
}

bool AoiViewSet::resetObserver(EntityId observerId, uint32_t expectedOldViewEpoch, uint32_t newViewEpoch)
{
    //确保该观察者有视野/视野世代小于新设置世代
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return false;
    if(vIt->second.clientViewEpoch != expectedOldViewEpoch || vIt->second.clientViewEpoch >= newViewEpoch)
        return false;
    
    auto& obSnapshot = vIt->second;
    initObserverSnapshot(obSnapshot, newViewEpoch);
    snapshotTasks_.erase(observerId);

    return true;
}

bool AoiViewSet::removeObserver(EntityId observerId, uint32_t expectedViewEpoch)
{
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return false;

    if(vIt->second.clientViewEpoch != expectedViewEpoch)
        return false;

    //?当目标观察者存在未处理事件时是否需要在处理完毕后才删除观察者视野
    vIt->second.committedState_.clear();
    vIt->second.pendings_.clear();
    vIt->second.needResync = false;

    views_.erase(vIt);
    snapshotTasks_.erase(observerId);
    return true;
}


//获取当前会话键和候选序号，不推进基线
std::optional<ViewSendToken> AoiViewSet::prepareSend(EntityId eid) const
{
    auto vIt = views_.find(eid);
    if(vIt == views_.end())
        return std::nullopt;

    // 分片快照发送期间冻结普通增量，避免序号及基线与固定快照交错。
    if(snapshotTasks_.contains(eid))
        return std::nullopt;

    ViewSendToken obState;
    initObserverState(vIt->second, obState, eid);
    return obState;
}


//查询,不存在则返回nullptr
const ObserverSnapshot* AoiViewSet::findObserver(EntityId observerId) const
{
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return nullptr;
    return &(vIt->second);
}

const SentEntityState* AoiViewSet::findSentState(EntityId observerId, EntityId targetId) const
{
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return nullptr;

    auto sesIt = vIt->second.committedState_.find(targetId);
    if(sesIt == vIt->second.committedState_.end())
        return nullptr;
    return &(sesIt->second);
}


//至少应接收：观察者 ID、准备时的会话键与序号、被接受消息实际携带的操作和实体状态
bool AoiViewSet::commitAccepted(EntityId observerId, 
                                const ViewSendToken& token,
                                const std::vector<AcceptedPush>& items)
{
    if(snapshotTasks_.contains(observerId))
        return false;
    //校验
    //1.观察者校验
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return false;
    //2.校验token
    if(token.observerId != observerId || 
       token.clientViewEpoch != vIt->second.clientViewEpoch || 
       token.nextViewSeq != vIt->second.nextViewSeq)
        return false;
    
    if(items.empty())
        return true;
    if(vIt->second.nextViewSeq == std::numeric_limits<uint64_t>::max())
        return false;

    // 分配、校验和操作都在副本中进行，失败不留下部分基线或 pending 变更。
    auto next = vIt->second;
    for(const auto& push : items)
    {
        if(push.targetId == 0 ||
           (push.state.kind != EntityKind::kPlayer && push.state.kind != EntityKind::kMonster &&
            push.state.kind != EntityKind::kNpc && push.state.kind != EntityKind::kDrop))
            return false;
        bool res = false;
        switch (push.kind) 
        {
        case AoiMsgKind::kEntry:
            res = processEntry(push, next);
            break;
        case AoiMsgKind::kMove:
            res = processMove(push, next);
            break;
        case AoiMsgKind::kLeave:
            res = processLeave(push, next);
            break;
        default:
            return false;
        }
        if(!res)
            return false;
        next.pendings_.erase(push.targetId);
    }
    auto& current = vIt->second;
    current.committedState_.swap(next.committedState_);
    current.pendings_.swap(next.pendings_);
    ++current.nextViewSeq;
    return true;
}


bool AoiViewSet::markPending(EntityId observerId, EntityId targetId, const ViewSendToken& token)
{
    //校验
    //1.观察者校验
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return false;
    //2.校验token
    if(token.observerId != observerId || 
       token.clientViewEpoch != vIt->second.clientViewEpoch || 
       token.nextViewSeq != vIt->second.nextViewSeq)
        return false;

    auto& entSet = vIt->second.pendings_;
    entSet.insert(targetId);
    return true;
}

//仅在目标已成功提交，或重新计算后确认不需要任何推送时清理
bool AoiViewSet::clearResolvedPending(EntityId observerId, EntityId targetId)
{
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return false;
    
    auto& map = vIt->second.pendings_;
    auto ses = map.find(targetId);
    if(ses == map.end())
        return false;;
    map.erase(ses);
    return true;
}


bool AoiViewSet::hasPending(EntityId observerId) const
{
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return false;
    
    auto& map = vIt->second.pendings_;
    if(map.size())
        return true;
    return false;
}

bool AoiViewSet::needResync(EntityId observerId) const
{
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return false;
    
    if(vIt->second.needResync)
        return true;
    return false;
}

const std::vector<EntityId> AoiViewSet::forEachObserver() const
{
    size_t sizew = views_.size();
    std::vector<EntityId> observerIds;
    observerIds.reserve(sizew);
    for(auto& kv : views_)
        observerIds.push_back(kv.first);
    return observerIds;
}

bool AoiViewSet::markNeedResync(EntityId observerId, uint32_t clientEpoch)
{
    auto vIt = views_.find(observerId);
    if(vIt == views_.end())
        return false;
    if(clientEpoch != vIt->second.clientViewEpoch)
        return false;
    if(vIt->second.resyncRevision == std::numeric_limits<uint32_t>::max())
        return false;
    
    vIt->second.needResync = true;
    vIt->second.resyncRevision++;
    snapshotTasks_.erase(observerId);
    return true;
}

AoiViewSet::SnapshotTask* AoiViewSet::findSnapshotTask(const ViewSnapshotToken& token)
{
    auto observer = views_.find(token.observerId);
    auto task = snapshotTasks_.find(token.observerId);
    if(observer == views_.end() || task == snapshotTasks_.end())
        return nullptr;
    const auto& state = observer->second;
    const auto& saved = task->second.token;
    if(!state.needResync || state.clientViewEpoch != token.clientViewEpoch ||
       state.resyncRevision != token.resyncRevision || state.nextViewSeq != token.viewSeq ||
       saved.snapshotId != token.snapshotId || saved.clientViewEpoch != token.clientViewEpoch ||
       saved.resyncRevision != token.resyncRevision || saved.viewSeq != token.viewSeq ||
       saved.partCount != token.partCount)
        return nullptr;
    return &task->second;
}

std::optional<ViewSnapshotToken> AoiViewSet::beginSnapshot(EntityId observerId,
                                                          const ViewBaseline& frozenBaseline,
                                                          uint32_t partCount)
{
    auto observer = views_.find(observerId);
    if(observer == views_.end() || !observer->second.needResync || partCount == 0 ||
       snapshotTasks_.contains(observerId) || nextSnapshotId_ == std::numeric_limits<uint64_t>::max() ||
       observer->second.nextViewSeq == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
    for(const auto& kv : frozenBaseline)
    {
        const auto kind = kv.second.kind;
        if(kv.first == 0 || (kind != EntityKind::kPlayer && kind != EntityKind::kMonster &&
                           kind != EntityKind::kNpc && kind != EntityKind::kDrop))
            return std::nullopt;
    }
    SnapshotTask task;
    task.token = {observerId, observer->second.clientViewEpoch, observer->second.nextViewSeq,
                  observer->second.resyncRevision, nextSnapshotId_, partCount};
    task.baseline = frozenBaseline;
    task.acceptedParts.assign(partCount, false);
    const auto token = task.token;
    snapshotTasks_.emplace(observerId, std::move(task));
    ++nextSnapshotId_;
    return token;
}

bool AoiViewSet::markSnapshotPartAccepted(const ViewSnapshotToken& token, uint32_t partIndex)
{
    auto* task = findSnapshotTask(token);
    if(task == nullptr || partIndex >= task->acceptedParts.size())
        return false;
    if(!task->acceptedParts[partIndex])
    {
        task->acceptedParts[partIndex] = true;
        ++task->acceptedCount;
    }
    return true;
}

bool AoiViewSet::commitSnapshot(const ViewSnapshotToken& token)
{
    auto* task = findSnapshotTask(token);
    if(task == nullptr || task->acceptedCount != token.partCount)
        return false;
    auto& observer = views_.find(token.observerId)->second;
    // swap 不分配；全部准备完成后一次提交，不留下半替换基线。
    observer.committedState_.swap(task->baseline);
    observer.pendings_.clear();
    ++observer.nextViewSeq;
    observer.needResync = false;
    snapshotTasks_.erase(token.observerId);
    return true;
}

bool AoiViewSet::abortSnapshot(const ViewSnapshotToken& token)
{
    if(findSnapshotTask(token) == nullptr)
        return false;
    snapshotTasks_.erase(token.observerId);
    return true;
}

bool AoiViewSet::references(EntityId targetId) const
{
    for(const auto& kv : views_)
        if(kv.second.committedState_.contains(targetId) || kv.second.pendings_.contains(targetId))
            return true;
    for(const auto& kv : snapshotTasks_)
        if(kv.second.baseline.contains(targetId))
            return true;
    return false;
}

bool AoiViewSet::references(EntityId targetId, uint32_t entityGeneration) const
{
    for(const auto& kv : views_)
    {
        if(kv.second.pendings_.contains(targetId))
            return true;
        const auto state = kv.second.committedState_.find(targetId);
        if(state != kv.second.committedState_.end() && state->second.entityGeneration == entityGeneration)
            return true;
    }
    for(const auto& kv : snapshotTasks_)
    {
        const auto state = kv.second.baseline.find(targetId);
        if(state != kv.second.baseline.end() && state->second.entityGeneration == entityGeneration)
            return true;
    }
    return false;
}



//----------------------------private----------------------------
bool AoiViewSet::processEntry(const AcceptedPush& push, ObserverSnapshot& obss)
{
    if(push.kind != AoiMsgKind::kEntry)
        return false;
    auto sesIt = obss.committedState_.find(push.targetId);
    if(sesIt == obss.committedState_.end())
    {
        obss.committedState_.emplace(push.targetId, push.state);
        return true;
    }
    const auto& old = sesIt->second;
    if(push.state.kind != old.kind || push.state.businessId != old.businessId ||
       push.state.entityGeneration < old.entityGeneration ||
       (push.state.entityGeneration == old.entityGeneration &&
        push.state.spatialVersion < old.spatialVersion))
        return false;
    sesIt->second = push.state;
    return true;
}

bool AoiViewSet::processMove(const AcceptedPush& push, ObserverSnapshot& obss)
{
    if(push.kind != AoiMsgKind::kMove)
        return false;
    auto sesIt = obss.committedState_.find(push.targetId);
    if(sesIt == obss.committedState_.end())
        return false;
    const auto& old = sesIt->second;
    if(push.state.kind != old.kind || push.state.businessId != old.businessId ||
       push.state.entityGeneration != old.entityGeneration ||
       push.state.spatialVersion < old.spatialVersion)
        return false;
    sesIt->second = push.state;
    return true;
}

bool AoiViewSet::processLeave(const AcceptedPush& push, ObserverSnapshot& obss)
{
    if(push.kind != AoiMsgKind::kLeave)
        return false;
    auto sesIt = obss.committedState_.find(push.targetId);
    if(sesIt == obss.committedState_.end())
        return true;
    const auto& old = sesIt->second;
    if(push.state.entityGeneration < old.entityGeneration)
        return true; // 旧世代 Leave 不能删除新世代。
    if(push.state.entityGeneration != old.entityGeneration ||
       push.state.kind != old.kind || push.state.businessId != old.businessId)
        return false;
    // 观察者移动也会产生 Leave，因此不要求目标空间版本比基线更大。
    obss.committedState_.erase(sesIt);
    return true;
}