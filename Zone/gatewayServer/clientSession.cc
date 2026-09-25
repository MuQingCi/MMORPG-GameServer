#include "clientSession.h"

uint64_t ClientSessionTable::Create(const TcpConnectionPtr& conn, uint8_t defaultDstService, int64_t nowMs)
{
    std::lock_guard<std::mutex> lk(mtx_);

    ClientSession s;
    s.sessionId = nextSessionId_++;
    s.dstService = defaultDstService;
    s.conn = conn;
    s.createdAtMs = nowMs;

    const uint64_t id = s.sessionId;
    sessions_.emplace(id, std::move(s));
    return id;
}

bool ClientSessionTable::Get(uint64_t sessionId, ClientSession& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end())
        return false;
    out = it->second;
    return true;
}

bool ClientSessionTable::FindByPlayer(uint64_t playerId, ClientSession& out) const
{
    if (playerId == 0)
        return false;

    std::lock_guard<std::mutex> lk(mtx_);
    auto pit = playerIndex_.find(playerId);
    if (pit == playerIndex_.end())
        return false;
    auto it = sessions_.find(pit->second);
    if (it == sessions_.end())
        return false;
    out = it->second;
    return true;
}

bool ClientSessionTable::Bind(uint64_t sessionId, uint64_t playerId, uint8_t dstService,
                              int64_t nowMs, uint64_t& kickedSessionId)
{
    kickedSessionId = 0;
    if (playerId == 0)
        return false;

    std::lock_guard<std::mutex> lk(mtx_);

    auto it = sessions_.find(sessionId);
    if (it == sessions_.end())
        return false;

    // 顶号：同一玩家的旧会话必须让位，否则旧连接会继续收到推送
    auto pit = playerIndex_.find(playerId);
    if (pit != playerIndex_.end() && pit->second != sessionId)
    {
        kickedSessionId = pit->second;
        auto old = sessions_.find(pit->second);
        if (old != sessions_.end())
        {
            // 旧会话降级为未鉴权：即使它还没被关闭，也不能再发起业务请求
            old->second.authed = false;
            old->second.playerId = 0;
        }
        playerIndex_.erase(pit);
    }

    const uint32_t epoch = ++playerEpoch_[playerId];
    it->second.playerId = playerId;
    it->second.dstService = dstService;
    it->second.authed = true;
    it->second.epoch = epoch;
    it->second.authedAtMs = nowMs;

    playerIndex_[playerId] = sessionId;
    return true;
}

bool ClientSessionTable::SetDstService(uint64_t sessionId, uint8_t dstService)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end())
        return false;
    it->second.dstService = dstService;
    return true;
}

bool ClientSessionTable::Erase(uint64_t sessionId)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end())
        return false;

    // 只有当索引仍指向本会话时才删（顶号后索引已指向新会话，不能误删）
    if (it->second.playerId != 0)
    {
        auto pit = playerIndex_.find(it->second.playerId);
        if (pit != playerIndex_.end() && pit->second == sessionId)
            playerIndex_.erase(pit);
    }
    sessions_.erase(it);
    return true;
}

size_t ClientSessionTable::Size() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return sessions_.size();
}
