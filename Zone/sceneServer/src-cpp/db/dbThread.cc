#include "db/dbThread.h"

#include "log/logger.h"

#include <utility>

namespace
{
// 指数退避：2s, 4s, 8s, 16s, 32s, 60s(封顶)
int64_t BackoffMs(uint32_t streak)
{
    int64_t ms = 2000;
    for (uint32_t i = 1; i < streak && ms < 60000; ++i)
        ms *= 2;
    return ms > 60000 ? 60000 : ms;
}
}  // namespace

DBThread::DBThread(MsgBus& bus, const DbConfig& cfg)
    : m_bus_(&bus), cfg_(cfg)
{
}

DBThread::~DBThread()
{
    stop();
}

bool DBThread::start()
{
    if (running_.load(std::memory_order_acquire))
        return true;

    stop_.store(false, std::memory_order_release);

    // ---- Redis 分片：每个分片一条线程 + 一个独占连接 ----
    // 分片数由配置决定；MsgBus 的 Redis 队列数必须与它一致（SceneServer 保证）。
    const size_t shardCount = cfg_.redis.ShardCount();
    redisShards_.clear();
    redisShards_.reserve(shardCount);

    for (size_t i = 0; i < shardCount; ++i)
    {
        auto shard = std::make_unique<RedisShard>();
        if (!cfg_.redis.EndpointOf(i, shard->host, shard->port))
        {
            LOG_ERROR << "redis shard " << i << " has invalid endpoint (expect ip:port)";
            return false;
        }
        shard->conn = std::make_unique<RedisConnection>();
        redisShards_.push_back(std::move(shard));
    }

    running_.store(true, std::memory_order_release);

    mysqlThread_ = std::thread([this] { mysqlLoop(); });
    for (size_t i = 0; i < redisShards_.size(); ++i)
        redisShards_[i]->thread = std::thread([this, i] { redisLoop(i); });

    LOG_INFO << "db threads started: mysql + redis shards=" << redisShards_.size();
    return true;
}

void DBThread::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;

    stop_.store(true, std::memory_order_release);

    if (mysqlThread_.joinable())
        mysqlThread_.join();

    for (auto& shard : redisShards_)
    {
        if (shard->thread.joinable())
            shard->thread.join();
    }

    mysql_.Close();
    for (auto& shard : redisShards_)
        shard->conn->Close();

    LOG_INFO << "db threads stopped, mysqlTasks=" << mysqlTasks_.load()
             << " redisTasks=" << redisTasks_.load()
             << " errors=" << errors_.load();
}

bool DBThread::redisConnected(size_t shard) const
{
    if (shard >= redisShards_.size())
        return false;
    return redisShards_[shard]->conn->IsConnected();
}

void DBThread::ensureMysqlConnection()
{
    const int64_t now = NowMs();

    if (mysql_.IsConnected())
    {
        mysqlFailStreak_ = 0;
        mysqlNextTryMs_ = 0;
        return;
    }
    if (now < mysqlNextTryMs_)
        return;

    if (mysql_.Connect(cfg_.mysql))
    {
        mysqlFailStreak_ = 0;
        mysqlNextTryMs_ = 0;
        return;
    }

    ++mysqlFailStreak_;
    const int64_t wait = BackoffMs(mysqlFailStreak_);
    mysqlNextTryMs_ = now + wait;
    LOG_WARNING << "mysql unavailable (streak=" << mysqlFailStreak_ << "), retry in " << wait << "ms";
}

void DBThread::ensureRedisConnection(size_t shard)
{
    if (shard >= redisShards_.size())
        return;

    RedisShard& s = *redisShards_[shard];
    const int64_t now = NowMs();

    if (s.conn->IsConnected())
    {
        s.failStreak = 0;
        s.nextTryMs = 0;
        return;
    }
    if (now < s.nextTryMs)
        return;

    // 每个分片连自己的端点：鉴权/超时等参数与单实例模式一致
    RedisConfig cfg = cfg_.redis;
    cfg.host = s.host;
    cfg.port = s.port;

    if (s.conn->Connect(cfg))
    {
        s.failStreak = 0;
        s.nextTryMs = 0;
        return;
    }

    ++s.failStreak;
    const int64_t wait = BackoffMs(s.failStreak);
    s.nextTryMs = now + wait;
    LOG_WARNING << "redis shard " << shard << " (" << s.host << ":" << s.port
                << ") unavailable (streak=" << s.failStreak << "), retry in " << wait << "ms";
}

void DBThread::replyResult(DbResultLedger& ledger, const Msg& task, const dbv::DbResult& res)
{
    Msg out;
    // 结果类型与任务通道一一对应：逻辑层据此决定走哪条回调
    out.head.msgType = (task.head.msgType == MsgType::MSGTYPE_DB_TASK_REDIS)
                           ? MsgType::MSGTYPE_DB_RESULT_REDIS
                           : MsgType::MSGTYPE_DB_RESULT_MYSQL;

    // 关键字段必须原样带回：ctx 用于找回 Lua 回调，epoch 用于校验 Actor 生命周期
    out.head.taskId = task.head.taskId;
    out.head.dbShard = task.head.dbShard;
    out.head.ctx = task.head.ctx;
    out.head.epoch = task.head.epoch;
    out.head.playerId = task.head.playerId;
    out.head.session = task.head.session;
    out.head.seq = task.head.seq;
    out.head.Module = task.head.Module;
    out.head.Method = task.head.Method;
    out.head.srcWorkerId = task.head.srcWorkerId;
    out.body = res.Encode();

    ledger.finish(task, std::move(out));
    flushResults(ledger);
}

void DBThread::flushResults(DbResultLedger& ledger)
{
    ledger.flush([this](const Msg& result) {
        return m_bus_->sendToWorker(result.head.srcWorkerId, result);
    });
}

void DBThread::mysqlLoop()
{
    LOG_INFO << "mysql channel thread running";

    Msg task;
    while (!stop_.load(std::memory_order_acquire))
    {
        processControls(mysqlResults_, false, 0);

        //处理事务重试
        mysqlRetries_.tick(NowMs(), [this](const dbv::DbTask& task, dbv::DbResult& result) {
            ensureMysqlConnection();
            mysql_.Execute(task,result);
        }, [this](const Msg& task, const dbv::DbResult& result) {
            replyResult(mysqlResults_,task,result);
        });

        flushResults(mysqlResults_);

        //检查结果账本容量
        if (!mysqlResults_.hasCapacity()) 
        {
            std::this_thread::sleep_for(kWaitMs);
            continue;
        }
        
        if (!m_bus_->waitPopDbMySql(task, kWaitMs))
        {
            // 空闲：借机维护连接（断开后由这里恢复，而不是每条 SQL 都重试）
            ensureMysqlConnection();
            continue;
        }

        const auto admission = mysqlResults_.accept(task);
        if (admission != DbResultLedger::Admission::New) 
        {
            if (admission == DbResultLedger::Admission::Conflict)
            {
                LOG_ERROR << "DB task identity conflict, taskId=" << task.head.taskId;

                dbv::DbResult conflict; 
                conflict.errcode = 1005;
                conflict.errmsg = "taskId reused with different request";
                conflict.classification = dbv::ResultClass::kIdentityConflict;

                Msg reply = task; 
                reply.head.msgType = MSGTYPE_DB_RESULT_MYSQL;
                reply.body = conflict.Encode();
                mysqlResults_.queueNotice(std::move(reply));
            }
            flushResults(mysqlResults_);
            continue;
        }

        //解码 DbTask
        dbv::DbTask dbTask;
        if (!dbv::DbTask::Decode(task.body, dbTask))
        {
            errors_.fetch_add(1, std::memory_order_relaxed);

            LOG_WARNING << "bad db task body, ctx=" << task.head.ctx;

            dbv::DbResult bad;
            bad.errcode = 1;
            bad.errmsg = "decode db task failed";
            replyResult(mysqlResults_, task, bad);
            continue;
        }

        ensureMysqlConnection();

        //执行 SQL 或事务
        dbv::DbResult res;
        mysql_.Execute(dbTask, res);
        if (res.errcode != 0)
        {
            errors_.fetch_add(1, std::memory_order_relaxed);

            LOG_WARNING << "mysql task failed, ctx=" << task.head.ctx << " err=" << res.errmsg;
        }

        mysqlTasks_.fetch_add(1, std::memory_order_relaxed);
        if (!mysqlRetries_.schedule(task,dbTask,res,1,NowMs()))
            //保存并回投结果
            replyResult(mysqlResults_, task, res);
    }

    // DB 停止前逻辑线程已完成排空；消费最后的结果 ACK。
    processControls(mysqlResults_, false, 0);
    LOG_INFO << "mysql channel thread stopped";
}

void DBThread::redisLoop(size_t shard)
{
    if (shard >= redisShards_.size())
        return;

    RedisShard& s = *redisShards_[shard];
    LOG_INFO << "redis shard " << shard << " thread running, endpoint=" << s.host << ":" << s.port;

    Msg task;
    while (!stop_.load(std::memory_order_acquire))
    {
        processControls(s.results, true, static_cast<uint16_t>(shard));
        flushResults(s.results);
        if (!s.results.hasCapacity()) {
            std::this_thread::sleep_for(kWaitMs);
            continue;
        }
        // 只消费属于本分片的队列：分片之间互不影响（一个实例慢不拖累其它实例）
        if (!m_bus_->waitPopDbRedis(static_cast<uint16_t>(shard), task, kWaitMs))
        {
            ensureRedisConnection(shard);
            continue;
        }

        const auto admission = s.results.accept(task);
        if (admission != DbResultLedger::Admission::New) {
            if (admission == DbResultLedger::Admission::Conflict) {
                LOG_ERROR << "Redis task identity conflict, taskId=" << task.head.taskId;
                dbv::DbResult conflict; conflict.errcode = 2004;
                conflict.errmsg = "taskId reused with different request";
                conflict.classification = dbv::ResultClass::kIdentityConflict;
                Msg reply = task; reply.head.msgType = MSGTYPE_DB_RESULT_REDIS;
                reply.body = conflict.Encode();
                s.results.queueNotice(std::move(reply));
            }
            flushResults(s.results);
            continue;
        }

        dbv::DbTask dbTask;
        if (!dbv::DbTask::Decode(task.body, dbTask))
        {
            errors_.fetch_add(1, std::memory_order_relaxed);
            dbv::DbResult bad;
            bad.errcode = 1;
            bad.errmsg = "decode redis task failed";
            replyResult(s.results, task, bad);
            continue;
        }

        ensureRedisConnection(shard);

        dbv::DbResult res;
        s.conn->Execute(dbTask, res);
        if (res.errcode != 0)
        {
            errors_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARNING << "redis task failed, shard=" << shard << " ctx=" << task.head.ctx
                        << " err=" << res.errmsg;
        }

        redisTasks_.fetch_add(1, std::memory_order_relaxed);
        replyResult(s.results, task, res);
    }

    processControls(s.results, true, static_cast<uint16_t>(shard));
    LOG_INFO << "redis shard " << shard << " thread stopped";
}

void DBThread::processControls(DbResultLedger& ledger, bool redis, uint16_t shard)
{
    Msg control;
    // 限制控制消息处理量，避免状态查询流量阻塞业务执行。
    for (size_t i = 0; i < 256 && m_bus_->tryPopDbControl(redis, shard, control); ++i) {
        const bool found = ledger.handleControl(control);
        if (control.head.dbControl == 2) {
            Msg status = control;
            status.head.msgType = MSGTYPE_DB_STATUS;
            // 0=未知或记录未保留，1=处理中，2=已完成（同时重投结果）。
            status.body.assign(1, static_cast<char>(found ? ledger.status(control.head.taskId) : 0));
            ledger.queueNotice(std::move(status));
        }
    }
}
