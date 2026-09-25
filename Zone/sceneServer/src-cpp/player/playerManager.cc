#include "player/playerManager.h"

#include "common/msg.h"
#include "log/logger.h"

#include <utility>

PlayerManager::PlayerManager(uint32_t workerId)
    : workerId_(workerId)
{
}

PlayerManager::~PlayerManager() = default;

Player* PlayerManager::getOrCreate(uint64_t playerId, uint64_t session, uint32_t& epochOut)
{
    if (playerId == 0)
    {
        epochOut = 0;
        return nullptr;
    }

    auto it = actors_.find(playerId);
    if (it != actors_.end())
    {
        Player& p = it->second;
        // 同会话重复处理消息：只刷新活跃时间，**不能递增 epoch**
        // （否则每条消息都会让在途的 DB/定时器回调被判为过期而全部丢弃）
        if (p.session() == session)
        {
            p.touch(NowMs());
            epochOut = p.epoch();
            return &p;
        }

        // 会话变了（重连/换网关）：换绑 session 并递增 epoch，
        // 让旧会话发起的异步回调全部失效，而不是写进新会话的状态里
        p.set_session(session);
        p.set_epoch(nextEpoch_++);
        p.touch(NowMs());
        epochOut = p.epoch();

        LOG_INFO << "player re-online (new session), pid=" << playerId << " session=" << session
                 << " epoch=" << p.epoch() << " worker=" << workerId_;
        return &p;
    }

    Player p;
    p.set_id(playerId);
    p.set_session(session);
    p.set_epoch(nextEpoch_++);
    p.touch(NowMs());

    auto res = actors_.emplace(playerId, std::move(p));
    // 更新跨线程可读的规模快照（owner 线程内写，见 playerManager.h 的说明）
    sizeSnapshot_.store(actors_.size(), std::memory_order_relaxed);
    epochOut = res.first->second.epoch();

    LOG_INFO << "player online, pid=" << playerId << " session=" << session
             << " epoch=" << epochOut << " worker=" << workerId_;
    return &res.first->second;
}

Player* PlayerManager::get(uint64_t playerId)
{
    auto it = actors_.find(playerId);
    return it == actors_.end() ? nullptr : &it->second;
}

const Player* PlayerManager::get(uint64_t playerId) const
{
    auto it = actors_.find(playerId);
    return it == actors_.end() ? nullptr : &it->second;
}

bool PlayerManager::remove(uint64_t playerId)
{
    auto it = actors_.find(playerId);
    if (it == actors_.end())
        return false;

    LOG_INFO << "player offline, pid=" << playerId << " epoch=" << it->second.epoch()
             << " worker=" << workerId_;
    actors_.erase(it);
    // 更新跨线程可读的规模快照（owner 线程内写）
    sizeSnapshot_.store(actors_.size(), std::memory_order_relaxed);
    return true;
}

bool PlayerManager::validateEpoch(uint64_t playerId, uint32_t epoch) const
{
    auto it = actors_.find(playerId);
    if (it == actors_.end())
        return false;

    // epoch == 0 视为"不校验"（C++ 侧发起、无 Actor 语义的内部调用）
    if (epoch == 0)
        return true;

    return it->second.epoch() == epoch;
}

uint32_t PlayerManager::epochOf(uint64_t playerId) const
{
    auto it = actors_.find(playerId);
    return it == actors_.end() ? 0 : it->second.epoch();
}

uint64_t PlayerManager::sessionOf(uint64_t playerId) const
{
    auto it = actors_.find(playerId);
    return it == actors_.end() ? 0 : it->second.session();
}
