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
    , mysql_control_queue_(std::make_unique<MsgQueue<Msg>>())
{
    if (numWorkers == 0)
        numWorkers = 1;  // 至少一个逻辑线程，否则后面取模会除零
    if (redisShards == 0)
        redisShards = 1;

    worker_queues_.reserve(numWorkers);
    workerEnqueuedClassCounts_.reserve(numWorkers);
    for (size_t i = 0; i < numWorkers; ++i)
    {
        worker_queues_.emplace_back(std::make_unique<MsgQueue<Msg>>());
        workerEnqueuedClassCounts_.emplace_back(std::make_unique<std::array<std::atomic<uint64_t>, 2>>());
    }

    redis_queues_.reserve(redisShards);
    for (size_t i = 0; i < redisShards; ++i)
    {
        redis_queues_.emplace_back(std::make_unique<MsgQueue<Msg>>());
        redis_control_queues_.emplace_back(std::make_unique<MsgQueue<Msg>>());
    }
}

MsgBus::~MsgBus() = default;

DispatchResult MsgBus::sendToActor(ActorId id, Msg m)
{
    const WorkerId wid = static_cast<WorkerId>(hashutil::WorkerForActor(id, worker_queues_.size()));
    return sendToWorker(wid, std::move(m));
}

DispatchResult MsgBus::sendToModule(ModuleId mid, Msg m)
{
    // 模块消息与 Actor 无关，直接按模块号取模：保证"同一模块的消息串行"
    const WorkerId wid = static_cast<WorkerId>(mid % worker_queues_.size());
    return sendToWorker(wid, std::move(m));
}

DispatchResult MsgBus::sendToWorkerByPlayerId(PlayerId pid, Msg m)
{
    // 玩家即 Actor：归属决策点全进程唯一，就在这一行
    const WorkerId wid = static_cast<WorkerId>(hashutil::WorkerForActor(pid, worker_queues_.size()));
    return sendToWorker(wid, std::move(m));
}

DispatchResult MsgBus::sendToWorker(WorkerId wid, Msg m)
{
    if (wid >= worker_queues_.size())
    {
        LOG_ERROR << "sendToWorker: wid=" << wid << " out of range, numWorker=" << worker_queues_.size();
        return diagnosticAdmission(numWorker()+2+redisShardCount(), DispatchResult::kInvalidTarget);
    }

    const uint16_t messageType = m.head.msgType;
    const bool business = messageType == MSGTYPE_CONN_DATA;
    m.diagnosticEnqueuedUs = diagnostics_ ? QueueDiagnostics::nowUs() : 0;
    const bool ok = worker_queues_[wid]->try_push(std::move(m));
    if (ok)
    {
        (*workerEnqueuedClassCounts_[wid])[business ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
    }
    if (!ok)
        LOG_WARNING << "sendToWorker: queue full, wid=" << wid;

    return diagnosticAdmission(wid, ok ? DispatchResult::kAccepted : DispatchResult::kBackpressure);
}

uint64_t MsgBus::enqueuedBusinessCount(WorkerId wid) const
{
    return wid < workerEnqueuedClassCounts_.size() ? (*workerEnqueuedClassCounts_[wid])[0].load(std::memory_order_relaxed) : 0;
}

uint64_t MsgBus::enqueuedInternalCount(WorkerId wid) const
{
    return wid < workerEnqueuedClassCounts_.size() ? (*workerEnqueuedClassCounts_[wid])[1].load(std::memory_order_relaxed) : 0;
}

std::vector<DispatchResult> MsgBus::broadcastToLogic(const Msg& m)
{
    std::vector<DispatchResult> results;
    results.reserve(numWorker());
    for (WorkerId wid = 0; wid < numWorker(); ++wid)
        results.push_back(sendToWorker(wid, m));
    return results;
}

DispatchResult MsgBus::broadcastToLogicExcept(WorkerId except, const Msg& m,
                                               std::vector<uint8_t>* perWorkerOk)
{
    if (except >= numWorker()) return DispatchResult::kInvalidTarget;
    if (perWorkerOk) perWorkerOk->assign(numWorker(), 0);
    auto result = DispatchResult::kAccepted;
    for (WorkerId wid = 0; wid < numWorker(); ++wid) {
        if (wid == except) continue;
        const auto dispatched = sendToWorker(wid, m);
        if (perWorkerOk) (*perWorkerOk)[wid] = IsAccepted(dispatched);
        if (!IsAccepted(dispatched)) result = dispatched;
    }
    return result;
}

DispatchResult MsgBus::sendToNet(Msg m)
{
    m.diagnosticEnqueuedUs = diagnostics_ ? QueueDiagnostics::nowUs() : 0;
    const bool ok = net_queue_->try_push(std::move(m));
    if (!ok)
    {
        LOG_WARNING << "sendToNet: queue full";
        return diagnosticAdmission(numWorker(),DispatchResult::kBackpressure);
    }

    // 入队成功才唤醒：空队列时的唤醒是纯浪费
    if (net_wakeup_)
        net_wakeup_();

    return diagnosticAdmission(numWorker(),DispatchResult::kAccepted);
}

void MsgBus::setNetWakeup(Wakeup cb)
{
    net_wakeup_ = std::move(cb);
}

DispatchResult MsgBus::sendToDb(Msg m)
{
    m.diagnosticEnqueuedUs = diagnostics_ ? QueueDiagnostics::nowUs() : 0;
    const bool redis = (m.head.msgType == MsgType::MSGTYPE_DB_TASK_REDIS);
    const bool mysql = (m.head.msgType == MsgType::MSGTYPE_DB_TASK_MYSQL);
    if (!redis && !mysql)
    {
        LOG_ERROR << "sendToDb: illegal msgType=" << MsgTypeName(m.head.msgType);
        return diagnosticAdmission(numWorker()+2+redisShardCount(),DispatchResult::kInvalidMessage);
    }

    if (!redis)
    {
        const bool save=m.diagnosticSaveTask;
        const auto worker=m.head.srcWorkerId;
        const auto begin=m.diagnosticEnqueuedUs;
        const auto id=m.head.taskId;
        const bool ok = mysql_queue_->try_push(std::move(m));
        if (!ok)
            LOG_WARNING << "sendToDb: mysql queue full";
        if(ok && diagnostics_ && save)
            diagnostics_->event(worker,"save_db_accepted",begin,QueueDiagnostics::nowUs(),id);
        return diagnosticAdmission(numWorker()+1,ok ? DispatchResult::kAccepted : DispatchResult::kBackpressure);
    }

    // Redis：按分片索引选队列。索引越界说明调用方算错了分片数
    // （例如配置改了 shards 数量而 MsgBus 没重建），必须明确报错而不是静默落到 0 号。
    const size_t shard = m.head.dbShard;
    if (shard >= redis_queues_.size())
    {
        LOG_ERROR << "sendToDb: redis shard " << shard << " out of range, shardCount="
                  << redis_queues_.size();
        return diagnosticAdmission(numWorker()+2+redisShardCount(),DispatchResult::kInvalidTarget);
    }

    const bool ok = redis_queues_[shard]->try_push(std::move(m));
    if (!ok)
        LOG_WARNING << "sendToDb: redis queue full, shard=" << shard;
    return diagnosticAdmission(numWorker()+2+shard,ok ? DispatchResult::kAccepted : DispatchResult::kBackpressure);
}

DispatchResult MsgBus::sendToDbControl(Msg m)
{
    if (!m.head.taskId || m.head.srcWorkerId >= numWorker() ||
        (m.head.dbControl != 1 && m.head.dbControl != 2)) return DispatchResult::kInvalidMessage;
    if (m.head.msgType == MSGTYPE_DB_CONTROL_MYSQL)
        return mysql_control_queue_->try_push(std::move(m)) ? DispatchResult::kAccepted : DispatchResult::kBackpressure;
    if (m.head.msgType != MSGTYPE_DB_CONTROL_REDIS) return DispatchResult::kInvalidMessage;
    if (m.head.dbShard >= redis_control_queues_.size()) return DispatchResult::kInvalidTarget;
    const auto shard = m.head.dbShard;
    return redis_control_queues_[shard]->try_push(std::move(m)) ? DispatchResult::kAccepted : DispatchResult::kBackpressure;
}

bool MsgBus::tryPopDbControl(bool redis, uint16_t shard, Msg& out)
{
    if (!redis) return mysql_control_queue_->try_pop(out);
    return shard < redis_control_queues_.size() && redis_control_queues_[shard]->try_pop(out);
}

bool MsgBus::tryPopWorker(WorkerId wid, Msg& out)
{
    if (wid >= worker_queues_.size())
    {
        LOG_ERROR << "tryPopWorker: wid=" << wid << " out of range";
        return false;
    }
    if (!worker_queues_[wid]->try_pop(out)) return false;
    recordDiagnosticPop(wid, out);
    return true;
}

bool MsgBus::waitPopWorker(WorkerId wid, Msg& out, std::chrono::milliseconds timeoutMs)
{
    if (wid >= worker_queues_.size())
    {
        LOG_ERROR << "waitPopWorker: wid=" << wid << " out of range";
        return false;
    }
    if (!worker_queues_[wid]->wait_pop_for(out, timeoutMs)) return false;
    recordDiagnosticPop(wid, out);
    return true;
}

void MsgBus::recordDiagnosticPop(WorkerId wid, const Msg& out)
{
    if (!diagnostics_) return;
    const auto type=out.head.msgType;
    const size_t category=type==MSGTYPE_CONN_DATA ? 0 :
        (type==MSGTYPE_DB_RESULT_MYSQL || type==MSGTYPE_DB_RESULT_REDIS) ? 1 : type==MSGTYPE_SERVICE ? 2 : 3;
    diagnostics_->dequeued(wid,category,out.diagnosticEnqueuedUs,QueueDiagnostics::nowUs());
}

bool MsgBus::tryPopNet(Msg& out)
{
    return diagnosticPop(numWorker(),out,net_queue_->try_pop(out));
}

bool MsgBus::waitPopNet(Msg& out, std::chrono::milliseconds timeoutMs)
{
    return diagnosticPop(numWorker(),out,net_queue_->wait_pop_for(out, timeoutMs));
}

bool MsgBus::tryPopDbMySql(Msg& out)
{
    return diagnosticPop(numWorker()+1,out,mysql_queue_->try_pop(out));
}

bool MsgBus::waitPopDbMySql(Msg& out, std::chrono::milliseconds timeoutMs)
{
    return diagnosticPop(numWorker()+1,out,mysql_queue_->wait_pop_for(out, timeoutMs));
}

bool MsgBus::tryPopDbRedis(uint16_t shard, Msg& out)
{
    if (shard >= redis_queues_.size())
        return false;
    return diagnosticPop(numWorker()+2+shard,out,redis_queues_[shard]->try_pop(out));
}

bool MsgBus::waitPopDbRedis(uint16_t shard, Msg& out, std::chrono::milliseconds timeoutMs)
{
    if (shard >= redis_queues_.size())
        return false;
    return diagnosticPop(numWorker()+2+shard,out,redis_queues_[shard]->wait_pop_for(out, timeoutMs));
}

DispatchResult MsgBus::diagnosticAdmission(size_t channel, DispatchResult result)
{
    if(diagnostics_) {
        const size_t kind=result==DispatchResult::kAccepted ? 0 : result==DispatchResult::kBackpressure ? 1 :
            result==DispatchResult::kInvalidTarget ? 2 : 3;
        diagnostics_->admission(channel,kind);
    }
    return result;
}

bool MsgBus::diagnosticPop(size_t channel, Msg& out, bool popped)
{
    if(popped && diagnostics_) diagnostics_->dequeued(channel,0,out.diagnosticEnqueuedUs,QueueDiagnostics::nowUs());
    return popped;
}

size_t MsgBus::depth(WorkerId wid) const
{
    if (wid >= worker_queues_.size())
        return 0;
    return worker_queues_[wid]->current_size();
}

size_t MsgBus::netDepth() const
{
    return net_queue_->current_size();
}

std::vector<size_t> MsgBus::workerDepths() const
{
    std::vector<size_t> result;
    result.reserve(worker_queues_.size());
    for (const auto& queue : worker_queues_)
        result.push_back(queue->current_size());
    return result;
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
