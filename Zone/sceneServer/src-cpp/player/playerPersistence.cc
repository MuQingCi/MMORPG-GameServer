#include "player/playerPersistence.h"

bool PlayerPersistence::request(Player& player, bool offline, const Submit& submit) 
{
    //1.判断是否存在记录
    //不存在则新建并初始化记录
    auto it = records_.find(player.id());
    if (it == records_.end()) 
    {
        if (records_.size() >= kMaxPlayers || !player.id()) 
            return false;
        Record r;
        r.actorEpoch = player.epoch();
        r.token = ++nextToken_;
        r.nextVersion = r.confirmedVersion = player.persistedVersion();
        r.confirmedRevision = player.persistedRevision();
        it = records_.emplace(player.id(),std::move(r)).first;
    }

    //此时一定存在一个记录
    auto& r = it->second;
    if (r.actorEpoch != player.epoch()) 
    {
        if (r.offline) //已下线
            return false;
        r.actorEpoch = player.epoch(); // 同一个存活 Player 更换客户端绑定。
    }

    r.offline = r.offline || offline;
    const auto revision = player.mutationRevision();
    if (r.active && r.active->capturedRevision == revision && !r.queued) 
        return true;
    if (r.queued && r.queued->capturedRevision == revision) 
        return true;
    if (!player.dirty() && revision == r.confirmedRevision && !r.active) 
    {
        if (offline) 
            records_.erase(it);
        return true;
    }
    if (r.nextVersion >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) 
    {
        r.state = State::Blocked; r.error = "save version exhausted"; return false;
    }
    
    //冻结快照
    auto snapshot = player.freezeSaveSnapshot(r.nextVersion+1,r.token);
    dbv::DbTask task; std::string error;
    if (!PlayerSnapshotDao::buildSave(snapshot,task,error)) 
    {
        r.error = error; 
        return false;
    }
    ++r.nextVersion;
    if (r.state == State::Idle) 
        r.taskId = 0; // 新的固定载荷使用新的请求身份。
    r.queued = std::move(snapshot);
    dispatch(r,submit);
    return true; // 快照已被接管，即使传递层暂时无法登记任务。
}

void PlayerPersistence::tick(const Submit& submit) 
{
    for (auto& [id,r] : records_) 
    { 
        (void)id; 
        dispatch(r,submit); 
    }
}

bool PlayerPersistence::owns(uint64_t taskId) const 
{
    if (!taskId) 
        return false;
    for (const auto& [id,r] : records_) 
    {
        (void)id; 
        if(r.active && r.taskId == taskId) 
            return true;
    }
    return false;
}

// 在 Actor epoch 校验前调用。只有明确成功才确认保存；
// 被新版本取代、冲突或未知结果都不能当作保存成功确认。
bool PlayerPersistence::complete(uint64_t taskId, const dbv::DbResult& result, Player* live) 
{
    auto it = records_.begin();
    while(it != records_.end() && it->second.taskId != taskId) 
        ++it;

    if(it == records_.end()) 
        return false;

    auto& r = it->second;
    if(r.state != State::AwaitResult || r.taskId != taskId || !r.active) 
        return false;

    const auto& snapshot = *r.active;
    // 早期解码或连接错误可能缺少快照元数据；
    // 仍需阻塞对应任务，绝不能静默丢弃保存责任。
    if(result.errcode || result.classification != dbv::ResultClass::kSuccess) 
    {
        r.state = State::Blocked;
        r.error = result.errmsg;
        return true;
    }

    if(result.playerId != snapshot.playerId || 
        result.saveVersion != snapshot.saveVersion ||
        result.capturedRevision != snapshot.capturedRevision || 
        result.saveLifecycleToken != r.token) 
    {
        r.state = State::Blocked; 
        r.error = "save result identity mismatch"; 
        return false;
    }

    r.confirmedVersion = snapshot.saveVersion;
    r.confirmedRevision = snapshot.capturedRevision;
    if(live && live->id() == result.playerId && live->epoch() == r.actorEpoch && !r.offline)
        live->confirmSave(r.confirmedVersion,r.confirmedRevision);

    r.active.reset(); 
    r.taskId = 0; 
    r.state = State::Idle;

    if(r.offline && !r.queued) 
        records_.erase(it);
    return true;
}

bool PlayerPersistence::blocksLogin(uint64_t playerId) const 
{
    auto it = records_.find(playerId);
    return it != records_.end() && it->second.offline;
}

const PlayerPersistence::Record* PlayerPersistence::find(uint64_t playerId) const 
{
    auto it = records_.find(playerId); 
    return it == records_.end() ? nullptr : &it->second;
}

bool PlayerPersistence::unsettled() const 
{
    for(const auto& [id,r]:records_) 
    {
        (void)id;
        if(r.state!=State::Idle || r.active || r.queued) 
            return true;
    }
    return false;
}