#include "service/aoi/aoiReplica.h"
#include "common/msg.h"
#include "service/aoi/aoiTypes.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace {
bool ValidEntityKind(EntityKind kind)
{
    return kind == EntityKind::kPlayer || kind == EntityKind::kMonster ||
           kind == EntityKind::kNpc || kind == EntityKind::kDrop;
}
}

AoiReplica::AoiReplica(workerId selfWorkerId,
                       const AoiConfig& config, 
                       const Map& map)
                     : wId_(selfWorkerId), 
                       config_(config), 
                       map_(map),
                       grid_(std::make_unique<AoiGrid>
                        (map.getMinX(), 
                         map.getMinY(), 
                          map.getCols(),
                          map.getRows(), 
                          map.getCellSize(),
                          config.viewRadiusCells()))
{
}

AoiReplica::~AoiReplica() = default;

AoiReplica::ApplyResult AoiReplica::Apply(const AoiEvent& ev, 
                                          Source src,
                                          std::unordered_set<uint64_t>& dirtyObservers)
{
    //校验消息来源类型
    if (src != Source::kLocal && src != Source::kRemote)
        return ApplyResult::kBadSource;
    //校验本地消息wId是否正确
    if ((src == Source::kLocal) != (ev.ownerThreadId == wId_))
        return ApplyResult::kBadSource;
    //校验消息类型、实例ID、实例类型是否合法
    if ((ev.msgKind != AoiMsgKind::kEntry && ev.msgKind != AoiMsgKind::kMove &&
         ev.msgKind != AoiMsgKind::kLeave) || ev.eId == 0 || !ValidEntityKind(ev.entityKind))
        return ApplyResult::kInvalid;

    //远端消息则依据快照水位判断是否需要忽略旧消息，如果没对应快照则跳过
    if (src == Source::kRemote) 
    {
        const auto water = ownerSnapshotSeq_.find(ev.ownerThreadId);
        if (water != ownerSnapshotSeq_.end() && ev.ownerSeq <= water->second)
            return ApplyResult::kIgnored;
    }

    const auto eIt = ents_.find(ev.eId);
    const auto tIt = tombstones_.find(ev.eId);

    //首版不支持 owner 迁移。通信信封中的可信发送者仍由 Service 校验
    //对应实例存在且线程Id不符 或 实例墓碑存在且墓碑内部线程id不符则拒绝
    if ((eIt != ents_.end() && eIt->second.ownerWorker != ev.ownerThreadId) ||
        (tIt != tombstones_.end() && tIt->second.ownerWorker != ev.ownerThreadId))
        return ApplyResult::kBadSource;
    //实例墓碑存在且墓碑世代大于消息则忽略
    if (tIt != tombstones_.end() && tIt->second.generation >= ev.entityGeneration)
        return ApplyResult::kIgnored;

    //实例存在
    if (eIt != ents_.end()) 
    {
        const auto& old = eIt->second;
        //若实例副本内部实例世代大于消息实例世代 或者 二者实例世代相等时，若副本内部空间世代更大则忽略
        if (ev.entityGeneration < old.entityGeneration ||
            (ev.entityGeneration == old.entityGeneration && ev.spatialVersion <= old.spatialVersion))
            return ApplyResult::kIgnored;
        //依据实例世代、实例种类、新旧Id是否相等判断消息是否合法
        if (ev.entityGeneration == old.entityGeneration &&
            (ev.entityKind != old.kind || ev.businessId != old.businessId))
            return ApplyResult::kInvalid;
    }

    uint32_t cellIdx = 0;
    if (ev.msgKind != AoiMsgKind::kLeave) 
    {
        const auto size = map_.getSize();
        //判断是否越界
        if (ev.target_x >= size.first || ev.target_y >= size.second)
            return ApplyResult::kOutOfBounds;
        const auto target = grid_->TryCellOf(ev.target_x, ev.target_y);
        if (!target) return ApplyResult::kOutOfBounds;
        cellIdx = *target;
    }
    // MOVE 不能隐式开始一个生命周期；上层据此安排 owner 快照或修复本地生命周期
    if (ev.msgKind == AoiMsgKind::kMove &&
        (eIt == ents_.end() || ev.entityGeneration != eIt->second.entityGeneration))
        return ApplyResult::kNeedSnapshot;

    if (src == Source::kLocal)
        processLocalAoiMsg(ev, cellIdx, dirtyObservers);
    else
        processRemoteAoiMsg(ev, cellIdx, dirtyObservers);
    return ApplyResult::kApplied;
}


void AoiReplica::addWatcher(uint64_t observerId, uint32_t cell)
{
    grid_->addWatcher(observerId, cell);
}

void AoiReplica::removeWatcher(uint64_t observerId, uint32_t cell)
{
    grid_->removeWatcher(observerId, cell);
}

//------------------查询相关------------------
//获取该观察者可以看见的所有实例Id数组
bool AoiReplica::getVisibleEntity(EntityId observerId, std::vector<AoiEntity>& out, bool includeSelf) const
{
    out.clear();
    auto obEntPtr = findById(observerId);
    if(obEntPtr == nullptr)
        return false;
    std::unordered_set<EntityId> seen;
    std::vector<uint32_t> watchCellIdx = grid_->getWatchCellIdx(obEntPtr->x, obEntPtr->y);
    for(auto idx : watchCellIdx)
    {
        for(auto eId : grid_->entities(idx))
        {
            if((!includeSelf && eId == observerId) || !seen.insert(eId).second)
                continue;
            const auto ent = ents_.find(eId);
            if(ent != ents_.end() && ent->second.cellIdx == idx)
                out.push_back(ent->second);
        }
    }
    return true;
}

const AoiCell& AoiReplica::cell(uint32_t cellIdx) const
{
    return grid_->getCell(cellIdx);
}

const AoiEntity* AoiReplica::findById(uint64_t id) const
{
    auto it = ents_.find(id);
    if(it == ents_.end())
        return nullptr;
    return &(it->second);
}

void AoiReplica::ForEachOwned(const std::function<void(const AoiEntity&)>& fn) const
{
    for(auto& kv : ents_)
    {
        if(kv.second.ownerWorker == wId_)
            fn(kv.second);
    }
}

AoiReplica::ApplyResult AoiReplica::ApplyOwnerSnapshot(const AoiOwnerSnapshot& snap, std::unordered_set<uint64_t>& dirtyObservers)
{
    //确保快照来自其他线程
    if (snap.ownerWokerId == wId_) return ApplyResult::kBadSource;

    //查询快照拥有线程id在本线程内是否拥有快照且快照版本小于当前快照
    const auto water = ownerSnapshotSeq_.find(snap.ownerWokerId);
    if (water != ownerSnapshotSeq_.end() && snap.seq <= water->second)
        return ApplyResult::kIgnored;

    //实例Id->实例
    std::unordered_map<EntityId, AoiEntity> incoming;
    const auto size = map_.getSize();
    //遍历快照实例
    for (const auto& ent : snap.ents) 
    {
        //1.先对实体进行身份校验
        //若当前实例未知 | 实例类型不合法 | 实例线程Id与快照不符 | 快照实例seq版本大于快照seq | incoming内部存在当前实例 则抛出非法
        if (ent.entityId == 0 || !ValidEntityKind(ent.kind) ||
            ent.ownerWorker != snap.ownerWokerId || ent.ownerSeq > snap.seq ||
            incoming.count(ent.entityId) != 0)
            return ApplyResult::kInvalid;

        //当前快照实例位置越界
        if (ent.x >= size.first || ent.y >= size.second)
            return ApplyResult::kOutOfBounds;

        //算出快照实例所处格子索引
        const auto cellIdx = grid_->TryCellOf(ent.x, ent.y);
        if (!cellIdx) return ApplyResult::kOutOfBounds;

        //若该实例Id存在旧副本、墓碑 则先获取二者引用
        //核实当前快照与前二者内部拥有线程id是否相符
        const auto old = ents_.find(ent.entityId);
        const auto tomb = tombstones_.find(ent.entityId);
        if ((old != ents_.end() && old->second.ownerWorker != snap.ownerWokerId) ||
            (tomb != tombstones_.end() && tomb->second.ownerWorker != snap.ownerWokerId))
            return ApplyResult::kBadSource;
        //确保当前快照内部实例的 seq >= 副本seq 且 (当前实例世代 > 副本世代 | 当前实例世代 = 副本世代且 (空间世代更高、实例种类相同、实例对应基本Id相同)) 
        if (old != ents_.end() && old->second.ownerSeq <= snap.seq &&
            (old->second.entityGeneration > ent.entityGeneration ||
             (old->second.entityGeneration == ent.entityGeneration &&
              (old->second.spatialVersion > ent.spatialVersion ||
               old->second.kind != ent.kind || old->second.businessId != ent.businessId))))
            return ApplyResult::kInvalid;
        //确保快照实例世代 > 墓碑世代
        if (tomb != tombstones_.end() && tomb->second.ownerSeq <= snap.seq &&
            tomb->second.generation >= ent.entityGeneration)
            return ApplyResult::kInvalid;

        //2.将实体信息标准化且更新实体格子索引并压入incoming;
        AoiEntity normalized = ent;
        normalized.cellIdx = *cellIdx;
        incoming.emplace(ent.entityId, normalized); //?用移动语义可以减少一次构造和析构？
    }

    // 所有分配和修改在临时状态进行；异常或验证失败不改变活动副本及水位 —— 下列操作均在临时副本内操作
    auto nextEnts = ents_;
    auto nextGrid = std::make_unique<AoiGrid>(*grid_);
    auto nextWater = ownerSnapshotSeq_;
    auto nextDirty = dirtyObservers;
    auto markCell = [&](uint32_t idx) {
        const auto& watchers = nextGrid->watchers(idx);
        nextDirty.insert(watchers.begin(), watchers.end());
    };
    //遍历临时副本，对已从快照中移除的临时实例副本所处格子内观察者打上标记，在对应格子内移除实体且从临时副本内清除
    for (auto it = nextEnts.begin(); it != nextEnts.end();) 
    {
        const auto& ent = it->second;
        if (ent.ownerWorker == snap.ownerWokerId && ent.ownerSeq <= snap.seq &&
            incoming.count(ent.entityId) == 0) 
        {
            markCell(ent.cellIdx);
            nextGrid->removeEntity(ent.entityId, ent.cellIdx);
            it = nextEnts.erase(it);
        } 
        else 
            ++it;
    }
    
    for (const auto& kv : incoming) 
    {
        const auto& ent = kv.second;
        const auto old = nextEnts.find(ent.entityId);
        const auto tomb = tombstones_.find(ent.entityId);
        //若incoming中的实例其对应的临时副本内的实例 seq大于快照seq 或者 incoming中的实例其对应的墓碑seq > 快照seq 则不操作
        if ((old != nextEnts.end() && old->second.ownerSeq > snap.seq) ||
            (tomb != tombstones_.end() && tomb->second.ownerSeq > snap.seq))
            continue;

        //存在对应的旧实例
        if (old != nextEnts.end()) 
        {
            markCell(old->second.cellIdx);
            nextGrid->removeEntity(ent.entityId, old->second.cellIdx);
        }
        //更新副本内的实例
        nextEnts[ent.entityId] = ent;
        nextGrid->addEntity(ent.entityId, ent.cellIdx);
        markCell(ent.cellIdx);
    }
    // 暂不回收墓碑：发送基线的引用检查尚未接入，不使用 TTL 代替正确性条件
    nextWater[snap.ownerWokerId] = snap.seq;
    ents_.swap(nextEnts);
    grid_.swap(nextGrid);
    dirtyObservers.swap(nextDirty);
    ownerSnapshotSeq_.swap(nextWater);
    return ApplyResult::kApplied;
}

//-------------------------private-------------------------

void AoiReplica::processLocalAoiMsg(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers)
{
    switch (ev.msgKind) 
    {
        case AoiMsgKind::kEntry:
            onLocalEntry(std::move(ev), cellIdx, dirtyObservers);
            break;
        case AoiMsgKind::kMove:
            onLocalMove(std::move(ev), cellIdx, dirtyObservers);
            break;
        case AoiMsgKind::kLeave:
            onLocalLeave(std::move(ev), cellIdx, dirtyObservers);
            break;
        case AoiMsgKind::kInvalid:
            break;
        default:
            break;
    }
}

void AoiReplica::processRemoteAoiMsg(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers)
{
    switch (ev.msgKind) 
    {
        case AoiMsgKind::kEntry:
            onRemoteEntry(std::move(ev), cellIdx, dirtyObservers);
            break;
        case AoiMsgKind::kMove:
            onRemoteMove(std::move(ev), cellIdx, dirtyObservers);
            break;
        case AoiMsgKind::kLeave:
            onRemoteLeave(std::move(ev), cellIdx, dirtyObservers);
            break;
        case AoiMsgKind::kInvalid:
            break;
        default:
            break;
    }
}

//本地进入
void AoiReplica::onLocalEntry(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers)
{
    auto it = ents_.find(ev.eId);
    if(it == ents_.end())
    {
        AoiEntity ent;
        //初始化ent;
        //.....
        updateEntity(ent, ev, cellIdx);
        ents_[ev.eId] = std::move(ent);
    }
    else //按正常流程，顶号/下线即立刻执行Leave并清理实例的，这个是兜底
    {
        //更新已有实例
        auto& ent = it->second;
        //先移除旧实例的所有关注格
        auto oldWatch = grid_->getWatchCellIdx(ent.x, ent.y);
        //TODO 需约定该步骤是否已经清除自身在自身格子的观察ID
        for(auto idx : oldWatch)
            grid_->removeWatcher(ev.eId, idx);
        grid_->removeEntity(ent.entityId, ent.cellIdx);

        //对旧格子观测者打标记
        auto& vec = grid_->watchers(ent.cellIdx);
        for(auto eId : vec)
            dirtyObservers.insert(eId);

        updateEntity(ent, ev, cellIdx);
    }
    //将实例ID添加到实例关注格watcher
    auto cellVec = grid_->getWatchCellIdx(ev.target_x, ev.target_y);
    //TODO 若addWatcher包含自身格子则不需要新增，若不包含则还需要手动添加
    for(auto cellIdx : cellVec)
        grid_->addWatcher(ev.eId, cellIdx);
    //注册实例
    grid_->addEntity(ev.eId, cellIdx);
    
    //获取该格子的所有watcherId并打标记
    auto& watcherVec = grid_->watchers(cellIdx);
    for(auto wIdx : watcherVec)
        dirtyObservers.insert(wIdx);

    //TODO 投递事件到其他逻辑线程

}

//本地移动-本地对象即观察者，需将旧Cell中本Id从watchers移除并在所有新watcheCell中添加本Id
void AoiReplica::onLocalMove(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers)
{
    //!明确未知Move直接拒绝，只放行已保存副本的实例Move

    //TODO 此处可获得旧观察者ID,新观察者ID,若需要发送消息可以在此处得到对应EID

    //1.先移除旧格事件
    auto entIt = ents_.find(ev.eId);
    if(entIt == ents_.end())
        return;
    
    //存在实例
    if(cellIdx != entIt->second.cellIdx)    //跨格子
    {
        //A.取旧格所有关注格子索引
        auto oldCellIdx = entIt->second.cellIdx;

        //TODO 还能优化，减少两次Vector构造，一次拷贝拷贝
        auto oldWatch = grid_->getWatchCellIdx(entIt->second.x, entIt->second.y);
        //B.移除旧位置的所有观察格子中自身的ID
        for(auto idx : oldWatch)
            grid_->removeWatcher(ev.eId, idx);
        
        grid_->removeEntity(ev.eId, oldCellIdx);

        //获取旧Cell中所有观察者ID并将其id添加到藏标记集合中
        auto& vec = grid_->watchers(oldCellIdx);
        for(auto id : vec)
            dirtyObservers.insert(id);
    }
    else //不跨格子
    {
        auto& ent = entIt->second;
        updateEntity(ent, ev, cellIdx);

        auto oldCellIdx = entIt->second.cellIdx;
        auto& vec = grid_->watchers(oldCellIdx);
        for(auto id : vec)
            dirtyObservers.insert(id);
        return;
    }

    //2.为新位置感兴趣格子添加自身实例Id
    auto newWatch = grid_->getWatchCellIdx(ev.target_x,ev.target_y);
    for(auto idx : newWatch)
        grid_->addWatcher(ev.eId, idx);
    
    //3.更新实例ID
    //TODO 待核验初始化/更新是否有缺漏
    auto& ent = entIt->second;
    updateEntity(ent, ev, cellIdx);

    grid_->addEntity(ev.eId, cellIdx);

    //4.获取新Cell中所有观察者ID并将其id添加到藏标记集合中
    //>放最后是因为当前观察者移动后也需要重新计算视野集合
    auto& vec = grid_->watchers(cellIdx);
    for(auto id : vec)
        dirtyObservers.insert(id);
}


//本地离开
void AoiReplica::onLocalLeave(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers)
{
    auto it = ents_.find(ev.eId);

    //未知实体
    if(it == ents_.end()) 
    {
        //留个墓碑
        Tombstone tomb;
        initTombstone(tomb, ev);
        tombstones_[ev.eId] = std::move(tomb);
        return;
    }

    //将该格观察者打上脏标记
    auto& vec = grid_->watchers(it->second.cellIdx);
    for(auto eId : vec)
        dirtyObservers.insert(eId);

    //TODO 还能优化，减少两次Vector构造，一次拷贝
    auto oldWatch = grid_->getWatchCellIdx(it->second.x, it->second.y);

    //移除旧位置的观察格子
    //TODO 需约定该步骤是否已经清除自身在自身格子的观察ID
    for(auto idx : oldWatch)
        grid_->removeWatcher(ev.eId, idx);

    //移除该实例
    grid_->removeEntity(ev.eId, it->second.cellIdx);
    grid_->removeWatcher(ev.eId, it->second.cellIdx);

    //留个墓碑
    Tombstone tomb;
    initTombstone(tomb, ev);
    tombstones_[ev.eId] = std::move(tomb);

    ents_.erase(it);
}

void AoiReplica::onRemoteEntry(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers)
{
    auto it = ents_.find(ev.eId);
    if(it == ents_.end())
    {
        AoiEntity ent;
        updateEntity(ent, ev, cellIdx);
        ents_[ev.eId] = std::move(ent);
    }
    else //按正常流程，顶号/下线即立刻执行Leave并清理实例的，这个是兜底
    {
        //获取已有实例
        auto& ent = it->second;
        
        //对旧格子观测者打标记
        auto& vec = grid_->watchers(ent.cellIdx);
        for(auto eId : vec)
            dirtyObservers.insert(eId);
        //移除现有实例所在格子
        grid_->removeEntity(ent.entityId, ent.cellIdx);
        //更新已有实例
        updateEntity(ent, ev, cellIdx);
    }
    
    //注册实例到新格
    grid_->addEntity(ev.eId, cellIdx);
    
    //获取该格子的所有watcherId并打标记
    auto& watcherVec = grid_->watchers(cellIdx);
    for(auto wIdx : watcherVec)
        dirtyObservers.insert(wIdx);
}

void AoiReplica::onRemoteMove(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers)
{
    //TODO 此处可获得旧观察者ID,新观察者ID,若需要发送消息可以在此处得到对应EID
    //更新新旧格子内部实例，保存新旧格子的观察者

    auto entIt = ents_.find(ev.eId);
    if(entIt == ents_.end())
        return;
    
    //存在实例
    //1.先移除旧格事件
    if(cellIdx != entIt->second.cellIdx)    //跨格子
    {
        //A.取旧格所有关注格子索引
        auto oldCellIdx = entIt->second.cellIdx;

        //B.获取旧Cell中所有观察者ID并将其id添加到藏标记集合中
        auto& vec = grid_->watchers(oldCellIdx);
        for(auto id : vec)
            dirtyObservers.insert(id);

        grid_->removeEntity(ents_[ev.eId].entityId, oldCellIdx);
    }
    else //不跨格子
    {
        auto& ent = entIt->second;
        updateEntity(ent, ev, cellIdx);

        auto oldCellIdx = entIt->second.cellIdx;
        auto& vec = grid_->watchers(oldCellIdx);
        for(auto id : vec)
            dirtyObservers.insert(id);
        return;
    }
    
    //2.更新实例ID
    //TODO 待核验初始化/更新是否有缺漏
    auto& ent = entIt->second;
    updateEntity(ent, ev, cellIdx);

    grid_->addEntity(ev.eId, cellIdx);

    //3.获取新Cell中所有观察者ID并将其id添加到藏标记集合中
    //>放最后是因为当前观察者移动后也需要重新计算视野集合
    auto& vec = grid_->watchers(cellIdx);
    for(auto id : vec)
        dirtyObservers.insert(id);
}

void AoiReplica::onRemoteLeave(const AoiEvent& ev, uint32_t cellIdx, std::unordered_set<uint64_t>& dirtyObservers)
{
    auto it = ents_.find(ev.eId);
    if(it == ents_.end()) 
    {
        //留个墓碑
        Tombstone tomb;
        initTombstone(tomb, ev);
        tombstones_[ev.eId] = std::move(tomb);
        return;
    }

    //将该格观察者打上脏标记
    auto& vec = grid_->watchers(it->second.cellIdx);
    for(auto eId : vec)
        dirtyObservers.insert(eId);

    //移除该实例
    grid_->removeEntity(ev.eId, it->second.cellIdx);

    //留个墓碑
    Tombstone tomb;
    initTombstone(tomb, ev);
    tombstones_[ev.eId] = std::move(tomb);

    ents_.erase(it);
}



void AoiReplica::onInvalid(const AoiEvent& ev, uint32_t cellIdx)
{
    
}

void AoiReplica::initTombstone(Tombstone& tomb, const AoiEvent& ev)
{
    tomb.generation = ev.entityGeneration;
    tomb.version    = ev.spatialVersion;
    tomb.ownerWorker= ev.ownerThreadId;
    tomb.atMs       = NowMs();
    tomb.ownerSeq   = ev.ownerSeq;
}


