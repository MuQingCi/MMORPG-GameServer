#include "common/msgBus.h"

#include "common/hash.h"
#include "common/msg.h"
#include "common/queue.h"
#include "log/logger.h"

#include <cstdint>
#include <utility>

namespace
{
// 队列积压深度的兜底比较：<=0 说明队列为空
inline bool LessLoaded(size_t a, size_t b) { return a < b; }
}  // namespace

MsgBus::MsgBus(size_t numWorkers, size_t redisShards)
    : net_queue_(std::make_unique<MsgQueue<Msg>>())
    , mysql_queue_(std::make_unique<MsgQueue<Msg>>())
{
    if (numWorkers == 0)
        numWorkers = 1;  // 至少一个逻辑线程，否则后面取模会除零
    if (redisShards == 0)
        redisShards = 1;

    worker_queues_.reserve(numWorkers);
    for (size_t i = 0; i < numWorkers; ++i)
        worker_queues_.emplace_back(std::make_unique<MsgQueue<Msg>>());

    redis_queues_.reserve(redisShards);
    for (size_t i = 0; i < redisShards; ++i)
        redis_queues_.emplace_back(std::make_unique<MsgQueue<Msg>>());
}

MsgBus::~MsgBus() = default;

bool MsgBus::sendToActor(ActorId id, Msg m)
{
    const WorkerId wid = static_cast<WorkerId>(hashutil::WorkerForActor(id, worker_queues_.size()));
    return sendToWorker(wid, std::move(m));
}

bool MsgBus::sendToModule(ModuleId mid, Msg m)
{
    // 模块消息与 Actor 无关，直接按模块号取模：保证"同一模块的消息串行"
    const WorkerId wid = static_cast<WorkerId>(mid % worker_queues_.size());
    return sendToWorker(wid, std::move(m));
}

bool MsgBus::sendToWorkerByPlayerId(PlayerId pid, Msg m)
{
    // 玩家即 Actor：归属决策点全进程唯一，就在这一行
    const WorkerId wid = static_cast<WorkerId>(hashutil::WorkerForActor(pid, worker_queues_.size()));
    return sendToWorker(wid, std::move(m));
}

bool MsgBus::sendToWorker(WorkerId wid, Msg m)
{
    if (wid >= worker_queues_.size())
    {
        LOG_ERROR << "sendToWorker: wid=" << wid << " out of range, numWorker=" << worker_queues_.size();
        return false;
    }

    const bool ok = worker_queues_[wid]->try_push(std::move(m));
    if (!ok)
        LOG_WARNING << "sendToWorker: queue full, wid=" << wid;

    return ok;
}

void MsgBus::broadcastToLogic(const Msg& m)
{
    //TODO 存在拷贝，后续可用shared_ptr指针来优化
    for (auto& q : worker_queues_)
    {
        Msg copy = m;  // 每条队列一份副本：Msg 不能跨线程共享同一对象
        if (!q->try_push(std::move(copy)))
            LOG_ERROR << "broadcastToLogic: try_push failed";
    }
}

bool MsgBus::sendToNet(Msg m)
{
    const bool ok = net_queue_->try_push(std::move(m));
    if (!ok)
    {
        LOG_WARNING << "sendToNet: queue full";
        return false;
    }

    // 入队成功才唤醒：空队列时的唤醒是纯浪费
    if (net_wakeup_)
        net_wakeup_();

    return true;
}

void MsgBus::setNetWakeup(Wakeup cb)
{
    net_wakeup_ = std::move(cb);
}

bool MsgBus::sendToDb(Msg m)
{
    const bool redis = (m.head.msgType == MsgType::MSGTYPE_DB_TASK_REDIS);
    const bool mysql = (m.head.msgType == MsgType::MSGTYPE_DB_TASK_MYSQL);
    if (!redis && !mysql)
    {
        LOG_ERROR << "sendToDb: illegal msgType=" << MsgTypeName(m.head.msgType);
        return false;
    }

    if (!redis)
    {
        const bool ok = mysql_queue_->try_push(std::move(m));
        if (!ok)
            LOG_WARNING << "sendToDb: mysql queue full";
        return ok;
    }

    // Redis：按分片索引选队列。索引越界说明调用方算错了分片数
    // （例如配置改了 shards 数量而 MsgBus 没重建），必须明确报错而不是静默落到 0 号。
    const size_t shard = m.head.dbShard;
    if (shard >= redis_queues_.size())
    {
        LOG_ERROR << "sendToDb: redis shard " << shard << " out of range, shardCount="
                  << redis_queues_.size();
        return false;
    }

    const bool ok = redis_queues_[shard]->try_push(std::move(m));
    if (!ok)
        LOG_WARNING << "sendToDb: redis queue full, shard=" << shard;
    return ok;
}

bool MsgBus::tryPopWorker(WorkerId wid, Msg& out)
{
    if (wid >= worker_queues_.size())
    {
        LOG_ERROR << "tryPopWorker: wid=" << wid << " out of range";
        return false;
    }
    return worker_queues_[wid]->try_pop(out);
}

bool MsgBus::waitPopWorker(WorkerId wid, Msg& out, std::chrono::milliseconds timeoutMs)
{
    if (wid >= worker_queues_.size())
    {
        LOG_ERROR << "waitPopWorker: wid=" << wid << " out of range";
        return false;
    }
    return worker_queues_[wid]->wait_pop_for(out, timeoutMs);
}

bool MsgBus::tryPopNet(Msg& out)
{
    return net_queue_->try_pop(out);
}

bool MsgBus::waitPopNet(Msg& out, std::chrono::milliseconds timeoutMs)
{
    return net_queue_->wait_pop_for(out, timeoutMs);
}

bool MsgBus::tryPopDbMySql(Msg& out)
{
    return mysql_queue_->try_pop(out);
}

bool MsgBus::waitPopDbMySql(Msg& out, std::chrono::milliseconds timeoutMs)
{
    return mysql_queue_->wait_pop_for(out, timeoutMs);
}

bool MsgBus::tryPopDbRedis(uint16_t shard, Msg& out)
{
    if (shard >= redis_queues_.size())
        return false;
    return redis_queues_[shard]->try_pop(out);
}

bool MsgBus::waitPopDbRedis(uint16_t shard, Msg& out, std::chrono::milliseconds timeoutMs)
{
    if (shard >= redis_queues_.size())
        return false;
    return redis_queues_[shard]->wait_pop_for(out, timeoutMs);
}

size_t MsgBus::depth(WorkerId wid) const
{
    if (wid >= worker_queues_.size())
        return 0;
    return worker_queues_[wid]->current_size();
}

MsgBus::WorkerId MsgBus::leastLoadedWorker(uint64_t tieBreak) const
{
    const size_t n = worker_queues_.size();
    if (n == 1)
        return 0;

    const WorkerId start = static_cast<WorkerId>(hashutil::SplitMix64(tieBreak) % n);

    WorkerId best = start;
    size_t bestDepth = depth(start);
    // 从哈希起点开始环形扫描，同分取哈希起点优先 -> 同分时天然打散
    for (size_t i = 1; i < n; ++i)
    {
        const WorkerId wid = static_cast<WorkerId>((start + i) % n);
        const size_t d = depth(wid);
        if (LessLoaded(d, bestDepth))
        {
            best = wid;
            bestDepth = d;
        }
    }
    return best;
}
