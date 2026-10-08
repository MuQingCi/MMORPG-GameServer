#include "service/aoi/aoiService.h"
#include "player/player.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
AoiEvent FromEntity(const AoiEntity& ent, AoiMsgKind kind)
{
    AoiEvent ev;
    ev.msgKind = kind;
    ev.entityKind = ent.kind;
    ev.eId = ent.entityId;
    ev.businessId = ent.businessId;
    ev.target_x = ent.x;
    ev.target_y = ent.y;
    ev.target_dir = ent.dir;
    ev.entityGeneration = ent.entityGeneration;
    ev.actorEpoch = ent.actorEpoch;
    ev.spatialVersion = ent.spatialVersion;
    ev.ownerThreadId = ent.ownerWorker;
    ev.ownerSeq = ent.ownerSeq;
    return ev;
}
}

AoiService::AoiService(workerId id, const AoiConfig& cfg, 
                       AoiPushSink& sink,
                       const Map& map, 
                       AoiServiceHooks hooks)
                    :  wId_(id), 
                       aoiConfig_(cfg), 
                       sink_(sink), 
                       map_(map), 
                       hooks_(std::move(hooks)),
                       replica_(id, aoiConfig_, map)
{
    //确保两个配置都合法
    std::string err;
    if(!cfg.Validate(err) || map.getCellSize() != cfg.cellSize || map.getCols() == 0)
        throw std::invalid_argument("invalid AOI configuration or map: " + err);

    //将所有逻辑线程ID排序并将重复的移出数据，然后将本线程id从数组中移出
    auto& targets = hooks_.publishTargets;
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    targets.erase(std::remove(targets.begin(), targets.end(), wId_), targets.end());
}

AoiService::ApplyResult AoiService::OnEntityEnter(Player& p) 
{ 
    return Enter(p, false); 
}

AoiService::ApplyResult AoiService::OnEntityReenter(Player& p) 
{ 
    return Enter(p, true); 
}


AoiService::ApplyResult AoiService::OnEntityLeave(EntityId id)
{
    //1.校验
    //校验实例是否存在
    const auto* ent = replica_.findById(id);
    if(!ent) 
        return ApplyResult::kIgnored;

    //校验是否来自本线程
    if(ent->ownerWorker != wId_) 
        return ApplyResult::kBadSource;

    //校验空间版本是否合法
    if(ent->spatialVersion == std::numeric_limits<uint64_t>::max()) 
        return ApplyResult::kInvalid;

    //2.依据实例信息生成AoiEvent事件并递增空间版本号
    auto ev = FromEntity(*ent, AoiMsgKind::kLeave);
    ++ev.spatialVersion;

    //3.获取应用本地事件的结果并根据结果执行相应处理
    const auto result = ApplyLocal(ev);
    if(result == ApplyResult::kApplied) 
    {
        const auto* observer = viewSet_.findObserver(id);
        if(observer) 
            viewSet_.removeObserver(id, observer->clientViewEpoch);
        dirtyObservers_.erase(id);
    }
    return result;
}

//此时的player是已经修改后的状态
AoiService::ApplyResult AoiService::OnLocalEntityMoved(Player& p)
{
    //1.校验
    const auto id = EntityOf(p.id());
    const auto* ent = id ? replica_.findById(*id) : nullptr;
    //校验实例
    if(!ent) 
        return ApplyResult::kNeedSnapshot;
    //校验目标位置
    if(!ValidPosition(p)) 
        return ApplyResult::kOutOfBounds;
    //比较移动前后实例信息
    if(ent->x == uint32_t(p.x()) && ent->y == uint32_t(p.y()) && ent->dir == p.dir())
        return ApplyResult::kIgnored;
    //校验空间版本号
    if(ent->spatialVersion == std::numeric_limits<uint64_t>::max()) 
        return ApplyResult::kInvalid;

    //2.依据实例信息生成AoiEvent并递增空间版本号
    auto ev = FromEntity(*ent, AoiMsgKind::kMove);
    ev.target_x = static_cast<Position>(p.x());
    ev.target_y = static_cast<Position>(p.y());
    ev.target_dir = p.dir();
    ev.actorEpoch = p.epoch();
    ++ev.spatialVersion;

    return ApplyLocal(ev);
}

AoiService::RemoteResult AoiService::onRemoteEntityEvent(workerId sender, const AoiEvent& ev)
{
    //1.拒绝本线程消息和非法消息
    if(sender == wId_ || sender != ev.ownerThreadId) 
        return RemoteResult::kRejected;

    //2.若远端消息为移动消息时
    if(ev.msgKind == AoiMsgKind::kMove) 
    {
        //校验消息数据是否合法
        const auto size = map_.getSize();
        if(ev.eId == 0 || ev.target_x >= size.first || ev.target_y >= size.second ||
           (ev.entityKind != EntityKind::kPlayer && ev.entityKind != EntityKind::kMonster &&
            ev.entityKind != EntityKind::kNpc && ev.entityKind != EntityKind::kDrop))
            return RemoteResult::kRejected;
        
        //判断是否存在实例ID相同的移动消息
        auto it = pendingRemoteMoves_.find(ev.eId);
        if(it != pendingRemoteMoves_.end()) 
        {
            const auto& old = it->second;
            //新旧消息所属线程不同则直接视为非法消息
            if(old.ownerThreadId != sender) 
                return RemoteResult::kRejected;

            //新消息为旧世代/旧空间版本消息则直接忽略
            if(ev.entityGeneration < old.entityGeneration ||
               (ev.entityGeneration == old.entityGeneration && ev.spatialVersion <= old.spatialVersion))
                return RemoteResult::kIgnored;
        }
        pendingRemoteMoves_[ev.eId] = ev;
        return RemoteResult::kQueued;
    }
    
    //3.远端消息非Move则直接应用，根据结果执行相应处理
    const auto result = replica_.Apply(ev, AoiReplica::Source::kRemote, dirtyObservers_);

    if(result == ApplyResult::kApplied) 
    {
        //该实例存在待处理Move事件，先校验二者信息，二者基本信息相等且新消息为新世代/新空间世代/Leave消息则直接清除对应待处理的Move
        auto it = pendingRemoteMoves_.find(ev.eId);
        if(it != pendingRemoteMoves_.end() && it->second.ownerThreadId == sender &&
           (it->second.entityGeneration < ev.entityGeneration ||
            (it->second.entityGeneration == ev.entityGeneration &&
             (ev.msgKind == AoiMsgKind::kLeave || it->second.spatialVersion <= ev.spatialVersion))))
            pendingRemoteMoves_.erase(it);
        return RemoteResult::kApplied;
    }
    return result == ApplyResult::kIgnored ? RemoteResult::kIgnored : RemoteResult::kRejected;
}


AoiService::ApplyResult AoiService::ApplyOwnerSnapshot(workerId sender, const AoiOwnerSnapshot& snap)
{
    //1.拒绝本线程消息和非法消息
    if(sender == wId_ || sender != snap.ownerWokerId) 
        return ApplyResult::kBadSource;

    //2.应用快照
    const auto result = replica_.ApplyOwnerSnapshot(snap, dirtyObservers_);
    if(result == ApplyResult::kApplied) 
    {
        for(auto it = pendingRemoteMoves_.begin(); it != pendingRemoteMoves_.end();) 
        {
            if(it->second.ownerThreadId == sender && it->second.ownerSeq <= snap.seq)
                it = pendingRemoteMoves_.erase(it);
            else ++it;
        }
        ownersNeedingSnapshot_.erase(sender);
    }
    return result;
}

AoiOwnerSnapshot AoiService::BuildOwnerSnapshot() const
{
    AoiOwnerSnapshot snap{ownerSeq_, wId_, {}};
    replica_.ForEachOwned([&](const AoiEntity& ent) { snap.ents.push_back(ent); });
    return snap;
}


//推送
AoiPushResult AoiService::pushToClient(const AoiClientPushRequest& request)
{
    return sink_.sendToClient(request);
}

AoiWorkerPublishResult AoiService::pushToWorkers(const AoiWorkerPublishRequest& request)
{
    return sink_.publishToWorkers(request);
}


bool AoiService::OnPlayerOnline(uint64_t pid, uint64_t gatewayId, uint32_t epoch)
{
    // 路由由会话组件/Sink 管理；这里仅管理观察者发送基线
    (void)gatewayId;
    const auto id = EntityOf(pid);
    if(!id || !replica_.findById(*id)) 
        return false;
    if(!viewSet_.registryObserver(*id, epoch)) 
        return false;
    dirtyObservers_.insert(*id);
    return true;
}

bool AoiService::OnPlayerRebind(uint64_t pid, uint32_t oldEpoch, uint32_t newEpoch)
{
    const auto id = EntityOf(pid);
    if(!id || !viewSet_.resetObserver(*id, oldEpoch, newEpoch)) 
        return false;
    dirtyObservers_.insert(*id);
    return true;
}

bool AoiService::OnPlayerOffline(uint64_t pid, uint32_t epoch, uint8_t reason)
{
    (void)reason;
    const auto id = EntityOf(pid);
    if(!id || !viewSet_.removeObserver(*id, epoch)) return false;
    dirtyObservers_.erase(*id);
    return true; // 会话离线不自动代表实体离开场景。
}


bool AoiService::OnViewResyncReq(uint64_t pid, uint32_t epoch)
{
    const auto id = EntityOf(pid);
    return id && OnClientSendFailed(*id, epoch);
}

bool AoiService::OnClientSendFailed(EntityId observer, uint32_t epoch)
{
    if(!viewSet_.markNeedResync(observer, epoch)) 
        return false;
    dirtyObservers_.insert(observer);
    return true;
}


void AoiService::OnTick(int64_t now)
{
    if(now < 0 || now < nextTickTime_) 
        return;
    nextTickTime_ = now > std::numeric_limits<int64_t>::max() - aoiConfig_.tickMs
        ? std::numeric_limits<int64_t>::max() : now + aoiConfig_.tickMs;
    ++tickCount_;
    FlushRemoteMoves();
    RetryPublishes();
    ProcessObservers();
}


AoiViewSet& AoiService::views() 
{ 
    return viewSet_; 
}

bool AoiService::ViewOf(EntityId observer, std::vector<AoiEntity>& out) const
{
    out.clear();
    return replica_.getVisibleEntity(observer, out);
}

AoiServiceStats AoiService::Stats() const
{
    return {pendingRemoteMoves_.size(), pendingPublishes_.size(), dirtyObservers_.size(),
            viewSet_.forEachObserver().size(), ownersNeedingSnapshot_.size(), tickCount_};
}

std::optional<EntityId> AoiService::EntityOf(uint64_t pid) const
{
    const auto it = identities_.find(pid);
    if(it == identities_.end()) 
        return std::nullopt;
    return it->second.id;
}

std::vector<workerId> AoiService::OwnersNeedingSnapshot() const
{
    return {ownersNeedingSnapshot_.begin(), ownersNeedingSnapshot_.end()};
}


//————————————————private————————————————
AoiService::ApplyResult AoiService::Enter(Player& p, bool reenter)
{
    //1.校验
    if(p.id() == 0) 
        return ApplyResult::kInvalid;
    if(!ValidPosition(p)) 
        return ApplyResult::kOutOfBounds;

    auto it = identities_.find(p.id());
    //陌生玩家
    if(it == identities_.end()) 
    {
        if(nextEntityCounter_ == std::numeric_limits<uint32_t>::max()) 
            return ApplyResult::kInvalid;

        //注册
        LocalIdentity identity;
        identity.id = (uint64_t(wId_) << 32) | nextEntityCounter_++;
        it = identities_.emplace(p.id(), identity).first;
    }

    auto& identity = it->second;
    //非重进且存在玩家状态，直接忽略
    if(!reenter && replica_.findById(identity.id)) 
        return ApplyResult::kIgnored;
    //非法实例世代
    if(identity.generation == std::numeric_limits<uint32_t>::max()) 
        return ApplyResult::kInvalid;

    //2.生成AoiEvent并应用
    AoiEvent ev;
    ev.msgKind = AoiMsgKind::kEntry;
    ev.entityKind = EntityKind::kPlayer;
    ev.eId = identity.id;
    ev.businessId = p.id();
    ev.entityGeneration = identity.generation + 1;
    ev.actorEpoch = p.epoch();
    ev.target_x = static_cast<Position>(p.x());
    ev.target_y = static_cast<Position>(p.y());
    ev.target_dir = p.dir();
    ev.spatialVersion = 1;
    ev.ownerThreadId = wId_;
    const auto result = ApplyLocal(ev);

    if(result == ApplyResult::kApplied) 
        identity.generation = ev.entityGeneration;
    return result;
}

AoiService::ApplyResult AoiService::ApplyLocal(const AoiEvent& event)
{
    //1.校验
    if(ownerSeq_ == std::numeric_limits<uint64_t>::max()) 
        return ApplyResult::kInvalid;
    
    //生成副本并使副本seq+1并将其加入到待发布队列中
    AoiEvent ev = event;
    ev.ownerSeq = ownerSeq_ + 1;
    pendingPublishes_.push_back({ev, hooks_.publishTargets});

    const auto result = replica_.Apply(ev, AoiReplica::Source::kLocal, dirtyObservers_);
    //应用失败则回退到执行前的状态
    if(result != ApplyResult::kApplied) 
        pendingPublishes_.pop_back();
    else 
    {   //成功时更新Seq
        ownerSeq_ = ev.ownerSeq;
        if(hooks_.publishTargets.empty()) 
            pendingPublishes_.pop_back();
    }
    return result;
}

void AoiService::FlushRemoteMoves()
{
    auto pending = std::move(pendingRemoteMoves_);
    pendingRemoteMoves_.clear();
    for(const auto& kv : pending) 
    {
        const auto result = replica_.Apply(kv.second, AoiReplica::Source::kRemote, dirtyObservers_);
        if(result == ApplyResult::kNeedSnapshot) 
            ownersNeedingSnapshot_.insert(kv.second.ownerThreadId);
    }
}

void AoiService::RetryPublishes()
{
    if(!hooks_.encodeEvent) 
        return;
    for(auto it = pendingPublishes_.begin(); it != pendingPublishes_.end();) 
    {
        AoiWorkerPublishRequest request;
        if(!hooks_.encodeEvent(it->event, request)) 
        { 
            ++it; 
            continue; 
        }
        request.targets = it->remaining;

        const auto result = sink_.publishToWorkers(request);
        std::vector<workerId> failed;
        for(auto target : it->remaining) 
        {
            const auto delivery = std::find_if(result.deliveries.begin(), result.deliveries.end(),
                [&](const WorkerDeliveryResult& r) { 
                    return r.target == target; });
            if(delivery == result.deliveries.end() || !delivery->accepted) 
                failed.push_back(target);
        }
        it->remaining.swap(failed);
        if(it->remaining.empty()) 
            it = pendingPublishes_.erase(it);
        else ++it;
    }
}

void AoiService::ProcessObservers()
{
    if(!hooks_.processObserver) 
        return;
    for(auto observer : viewSet_.forEachObserver()) 
    {
        if(!dirtyObservers_.contains(observer) && !viewSet_.hasPending(observer) &&
           !viewSet_.needResync(observer)) 
            continue;
        if(hooks_.processObserver(*this, observer)) 
            dirtyObservers_.erase(observer);
    }
}

bool AoiService::ValidPosition(const Player& p) const 
{ 
    return map_.contains(p.x(), p.y()); 
}