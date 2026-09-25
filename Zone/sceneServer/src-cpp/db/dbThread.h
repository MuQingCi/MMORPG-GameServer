#ifndef CLEARMOON_DB_DBTHREAD_H
#define CLEARMOON_DB_DBTHREAD_H

#include "common/msg.h"
#include "common/msgBus.h"
#include "db/dbConfig.h"
#include "db/dbValue.h"
#include "db/mysqlConnection.h"
#include "db/redisConnection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

/**
 * @brief DB 线程组：MySQL 通道 + Redis 通道，各自一条线程、一个独占连接
 *
 * 为什么是"两条线程"而不是"一条线程两个连接"：
 *   Redis 是亚毫秒级、MySQL 是毫秒到几十毫秒级。如果共用一个消费线程，
 *   一条慢 SQL 会把后面所有 Redis 读全部堵死（设计文档 3.5 明确要求分通道）。
 *   两条独立线程 + 两条独立队列后，慢 SQL 只影响 MySQL 通道自身。
 *
 * 三条硬约束（违背就会出现随机崩溃/数据错乱）：
 *   1. **DB 线程绝不碰 lua_State**：Lua 回调只能由 owner 逻辑线程执行。
 *      正确路径：DB线程 -> MsgBus::sendToWorker(srcWorkerId, DB_RESULT) -> 逻辑线程进 Lua；
 *   2. **结果必须带 epoch**：异步返回时发起方可能已下线/Actor 已重建，
 *      逻辑层用 epoch 校验，不匹配就丢弃，绝不写回新生命周期的对象；
 *   3. **落库必须幂等**：SQL 一律用 INSERT ... ON DUPLICATE KEY UPDATE 或带 version
 *      的乐观锁 —— 因为超时重试一定会发生。
 */
class DBThread
{
public:
    DBThread(MsgBus& bus, const DbConfig& cfg);
    ~DBThread();

    DBThread(const DBThread&) = delete;
    DBThread& operator=(const DBThread&) = delete;

    bool start();
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }
    bool mysqlConnected() const { return mysql_.IsConnected(); }
    // Redis 分片数 = 配置里的 shards 数量（未配则为 1）
    size_t redisShardCount() const { return redisShards_.size(); }
    bool redisConnected(size_t shard) const;

    uint64_t mysqlTasks() const { return mysqlTasks_.load(std::memory_order_relaxed); }
    uint64_t redisTasks() const { return redisTasks_.load(std::memory_order_relaxed); }
    uint64_t errors() const { return errors_.load(std::memory_order_relaxed); }

private:
    // 一个 Redis 分片 = 一条队列（MsgBus）+ 一条线程 + 一个独占连接
    struct RedisShard
    {
        std::unique_ptr<RedisConnection> conn;
        std::thread thread;

        std::string host;      // 本分片端点（来自 RedisConfig::EndpointOf）
        uint16_t port = 0;

        int64_t  nextTryMs = 0;
        uint32_t failStreak = 0;
    };

    void mysqlLoop();
    void redisLoop(size_t shard);

    // 把结果投回"发起它的逻辑线程"
    void replyResult(const Msg& task, const dbv::DbResult& res);

    // 连接维护：失联时按**指数退避**重试（避免"每 2 秒刷一条 ERROR"的日志噪音，
    // 也避免 DB 恢复瞬间被打满连接风暴）
    void ensureMysqlConnection();
    void ensureRedisConnection(size_t shard);

    MsgBus* m_bus_ = nullptr;
    DbConfig cfg_;

    MySqlConnection mysql_;
    std::vector<std::unique_ptr<RedisShard>> redisShards_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread mysqlThread_;

    int64_t  mysqlNextTryMs_ = 0;
    uint32_t mysqlFailStreak_ = 0;

    std::atomic<uint64_t> mysqlTasks_{0};
    std::atomic<uint64_t> redisTasks_{0};
    std::atomic<uint64_t> errors_{0};

    static constexpr int64_t kMinReconnectMs = 2000;    // 首次失败后 2s
    static constexpr int64_t kMaxReconnectMs = 60000;   // 退避上限 60s
    static constexpr std::chrono::milliseconds kWaitMs{100};
};

#endif
