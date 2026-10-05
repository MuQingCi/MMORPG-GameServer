#include "service/aoi/aoiReplica.h"
#include "common/msg.h"
#include "service/aoi/aoiTypes.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

AoiReplica::AoiReplica(workerId selfWorkerId, 
                       const AoiConfig& config, 
                       const Map& map) 
                     : wId_(selfWorkerId),
                       config_(config),
                       map_(map),
                       grid_(std::make_unique<AoiGrid>(map.getMinX(),map.getMinY(),map.getCols(),map.getRows(),map.getCellSize(),config.viewRadiusCells()))
{

}

AoiReplica::~AoiReplica()
{

}

AoiReplica::ApplyResult AoiReplica::Apply(const AoiEvent& ev, Source src, std::unordered_set<uint64_t>& dirtyObservers)
{
    if(ev.msgKind == AoiMsgKind::kInvalid)
        return ApplyResult::kInvalid;
    //TODO 身份校验还未实现，思绪有点混乱

    //若到达事件为离开事件，则先处理离开事件------//TODO 不知道是否需要添加世代校验

    //TODO 可以优化，对非Leave消息才判断边界
    //计算是否越界
    auto target_x = ev.target_x;
    auto target_y = ev.target_y;
    auto target_cell = grid_->TryCellOf(target_x,target_y);
    if(ev.msgKind != AoiMsgKind::kLeave)
    {
        if(!target_cell.has_value())
            return ApplyResult::kOutOfBounds;
    }

    auto cellIdx = target_cell.value();
    auto eIt = ents_.find(ev.eId);
    auto tIt = tombstones_.find(ev.eId);
    switch (src) 
    {
        //本地消息默认永远为最新
        case Source::kLocal: 
            processLocalAoiMsg(std::move(ev), cellIdx, dirtyObservers);
            return ApplyResult::kApplied;            
            break;
        //跨线程投递为异步，所以需要校验世代版本且合并Move事件
        case Source::kRemote:
            //TODO 先校验身份再处理进入事件,暂时以实例世代，空间世代为依据，并未考虑ActorEpoch
            //校验身份
            //1.当前事件对应实例存在墓碑，
            //2.墓碑实例对应实例世代 >= 消息实例世代时直接拒绝
            //TODO 还可以依据atMs,Seq判断消息
            if(tIt != tombstones_.end())
            {
                if(tIt->second.generation > ev.entityGeneration || tIt->second.generation == ev.entityGeneration)
                    return ApplyResult::kIgnored;   //旧实例消息
                // //墓碑实例对应实例世代 和 消息实例世代 相等时直接拒绝
                // else if(tIt->second.generation == ev.entityGeneration)
                //     //墓碑实例空间世代和当前消息实例世代
                //     if(tIt->second.version >= ev.spatialVersion)
                //         return ApplyResult::kIgnored;   //旧空间消息
            } 
            //2.判断当前实例副本内部实例/空间世代是否高于消息世代
            if(eIt != ents_.end())
            {
                if(eIt->second.entityGeneration > ev.entityGeneration)
                    return ApplyResult::kIgnored;   //旧世代消息

                //比较副本实例世代与消息实例世代
                else if(eIt->second.entityGeneration == ev.entityGeneration)
                    if(eIt->second.spatialVersion >= ev.spatialVersion)
                        return ApplyResult::kIgnored; //旧空间消息
            }
            //3.到达此处的消息，满足
            //A.为最新消息且墓碑不存在/墓碑较旧

            processRemoteAoiMsg(std::move(ev), cellIdx, dirtyObservers);
            
            return ApplyResult::kApplied;   
            break;
        default:
            break;
    }
    return ApplyResult::kBadSource;
}


void AoiReplica::addWatcher(uint64_t observerId, uint32_t cell)
{
    grid_->addWatcher(observerId, cell);
}

void AoiReplica::removeWatcher(uint64_t observerId, uint32_t cell)
{
    grid_->removeWatcher(observerId, cell);
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
        fn(kv.second);
    }
}

void AoiReplica::ApplyOwnerSnapshot(const AoiOwnerSnapshot& snap)
{

}

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
    //!应该明确未知Move是自动创建实例，还是直接拒绝，只放行以保存副本的实例Move

    //TODO 此处可获得旧观察者ID,新观察者ID,若需要发送消息可以在此处得到对应EID

    //1.先移除旧格事件
    auto entIt = ents_.find(ev.eId);
    if(entIt != ents_.end())
    {
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
        
    }
    
    //2.为新位置感兴趣格子添加自身实例Id
    auto newWatch = grid_->getWatchCellIdx(ev.target_x,ev.target_y);
    for(auto idx : newWatch)
        grid_->addWatcher(ev.eId, idx);
    
    //3.更新实例ID
    auto it = ents_.find(ev.eId);
    //TODO 待核验初始化/更新是否有缺漏
    if(it == ents_.end())
    {
        AoiEntity ent;
        updateEntity(ent, ev, cellIdx);
        ents_[ev.eId] = std::move(ent);
    }
    else {
        auto& ent = it->second;
        updateEntity(ent, ev, cellIdx);
    }
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
        Tombstone tob;
        tob.generation  = ev.entityGeneration;
        tob.version     = ev.spatialVersion;
        tob.atMs        = NowMs();
        tob.ownerSeq    = ev.ownerSeq;
        tob.ownerWorker = ev.ownerThreadId;
        tombstones_[ev.eId] = std::move(tob);
        return;
    }

    //TODO 还能优化，减少两次Vector构造，一次拷贝拷贝
    auto oldWatch = grid_->getWatchCellIdx(it->second.x, it->second.y);
    //移除旧位置的观察格子
    //TODO 需约定该步骤是否已经清除自身在自身格子的观察ID
    for(auto idx : oldWatch)
        grid_->removeWatcher(ev.eId, idx);

    //添加墓碑
    Tombstone tob;
    tob.generation = ev.entityGeneration;
    tob.version    = ev.spatialVersion;
    tob.ownerWorker= ev.ownerThreadId;
    tob.atMs       = NowMs();
    tob.ownerSeq   = ev.ownerSeq;
    tombstones_[ev.eId] = std::move(tob);

    //移除该实例
    grid_->removeEntity(ev.eId, cellIdx);
    grid_->removeWatcher(ev.eId, cellIdx);
    ents_.erase(it);

    //将该格观察者打上脏标记
    auto& vec = grid_->watchers(cellIdx);
    for(auto eId : vec)
        dirtyObservers.insert(eId);
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

    //1.先移除旧格事件
    auto entIt = ents_.find(ev.eId);
    if(entIt != ents_.end())
    {
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
    }
    
    //2.更新实例ID
    //!第二次校验ents，可以优化成一次
    auto it = ents_.find(ev.eId);
    //TODO 待核验初始化/更新是否有缺漏
    if(it == ents_.end())
    {
        AoiEntity ent;
        updateEntity(ent, ev, cellIdx);
        ents_[ev.eId] = std::move(ent);
    }
    else {
        auto& ent = it->second;
        updateEntity(ent, ev, cellIdx);
    }
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
        Tombstone tob;
        tob.generation  = ev.entityGeneration;
        tob.version     = ev.spatialVersion;
        tob.atMs        = NowMs();
        tob.ownerSeq    = ev.ownerSeq;
        tob.ownerWorker = ev.ownerThreadId;
        tombstones_[ev.eId] = std::move(tob);
        return;
    }

    //移除该实例
    grid_->removeEntity(ev.eId, cellIdx);

    //添加墓碑
    Tombstone tob;
    tob.generation = ev.entityGeneration;
    tob.version    = ev.spatialVersion;
    tob.ownerWorker= ev.ownerThreadId;
    tob.atMs       = NowMs();
    tob.ownerSeq   = ev.ownerSeq;
    tombstones_[ev.eId] = std::move(tob);

    ents_.erase(it);

    //将该格观察者打上脏标记
    auto& vec = grid_->watchers(cellIdx);
    for(auto eId : vec)
        dirtyObservers.insert(eId);
}



void AoiReplica::onInvalid(const AoiEvent& ev, uint32_t cellIdx)
{
    
}


