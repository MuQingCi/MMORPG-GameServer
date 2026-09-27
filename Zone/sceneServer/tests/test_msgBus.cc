#include "common/hash.h"
#include "common/msg.h"
#include "common/msgBus.h"
#include "test_util.h"

#include <set>
#include <thread>
#include <vector>

// 归属必须是**纯函数且稳定**：同一 playerId 在任何时候都落到同一线程
TEST(HashStableForSameActor)
{
    const size_t n = 8;
    const uint64_t pid = 123456789;
    const uint32_t w1 = hashutil::WorkerForActor(pid, n);
    for (int i = 0; i < 100; ++i)
        CHECK_EQ(hashutil::WorkerForActor(pid, n), w1);

    CHECK_EQ(hashutil::WorkerForActor(pid, n), hashutil::WorkerForActor(pid, n));
}

// 玩家 id 若是"低位数位为 0"的结构化输入，取模会严重倾斜；stable hash 必须能打散
TEST(HashSpreadsStructuredIds)
{
    const size_t n = 8;
    std::set<uint32_t> used;
    for (uint64_t i = 0; i < 64; ++i)
    {
        // 模拟 "zone << 32 | platform << 16 | seq" 这种低位全是 0 的 id
        const uint64_t pid = ((i + 1) << 32) | 0x1000;
        used.insert(hashutil::WorkerForActor(pid, n));
    }
    // 64 个 id 至少要落到 6 个不同线程上（取模会全部落到 0）
    CHECK(used.size() >= 6);
}

TEST(MsgBusRouteByPlayerId)
{
    MsgBus bus(4);

    for (uint64_t pid = 1; pid <= 100; ++pid)
    {
        Msg m;
        m.head.msgType = MsgType::MSGTYPE_CONN_DATA;
        m.head.playerId = pid;
        m.head.Module = Module::PLAYER;
        CHECK(bus.sendToWorkerByPlayerId(pid, m));
    }

    // 每条消息都应出现在 hash(pid) 对应的队列里
    size_t total = 0;
    for (uint32_t w = 0; w < 4; ++w)
    {
        Msg out;
        while (bus.tryPopWorker(w, out))
        {
            CHECK_EQ(w, hashutil::WorkerForActor(out.head.playerId, 4));
            ++total;
        }
    }
    CHECK_EQ(total, (size_t)100);
}

TEST(MsgBusDbChannelsAreSeparate)
{
    MsgBus bus(2, 1);

    Msg mysqlTask;
    mysqlTask.head.msgType = MsgType::MSGTYPE_DB_TASK_MYSQL;
    mysqlTask.head.ctx = 11;
    CHECK(bus.sendToDb(mysqlTask));

    Msg redisTask;
    redisTask.head.msgType = MsgType::MSGTYPE_DB_TASK_REDIS;
    redisTask.head.ctx = 22;
    CHECK(bus.sendToDb(redisTask));

    // 两条通道各自独立：Redis 任务不会出现在 MySQL 通道里（慢 SQL 不该堵住 Redis）
    Msg out;
    CHECK(bus.tryPopDbMySql(out));
    CHECK_EQ(out.head.ctx, (uint64_t)11);
    CHECK(!bus.tryPopDbMySql(out));

    CHECK(bus.tryPopDbRedis(0, out));
    CHECK_EQ(out.head.ctx, (uint64_t)22);
    CHECK(!bus.tryPopDbRedis(0, out));
}

// Redis 多实例分片：任务必须落到 dbShard 指定的那条队列
TEST(MsgBusRedisShardRouting)
{
    constexpr size_t kShards = 3;
    MsgBus bus(1, kShards);
    CHECK_EQ(bus.redisShardCount(), kShards);

    for (uint16_t shard = 0; shard < kShards; ++shard)
    {
        Msg m;
        m.head.msgType = MsgType::MSGTYPE_DB_TASK_REDIS;
        m.head.dbShard = shard;
        m.head.ctx = 100 + shard;
        CHECK(bus.sendToDb(m));
    }

    // 每个分片各取到自己的那一条，且互不串号
    for (uint16_t shard = 0; shard < kShards; ++shard)
    {
        Msg out;
        CHECK(bus.tryPopDbRedis(shard, out));
        CHECK_EQ(out.head.dbShard, shard);
        CHECK_EQ(out.head.ctx, (uint64_t)(100 + shard));
        CHECK(!bus.tryPopDbRedis(shard, out));
    }

    // 分片索引越界必须被拒绝（而不是静默投到 0 号队列）
    Msg bad;
    bad.head.msgType = MsgType::MSGTYPE_DB_TASK_REDIS;
    bad.head.dbShard = static_cast<uint16_t>(kShards);
    CHECK(!bus.sendToDb(bad));
}

// MPSC：多生产者 -> 单消费者，条数不超过队列容量时一条都不能丢
TEST(MsgBusMultiProducer)
{
    MsgBus bus(1);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;   // 4*200 = 800 < 队列初始容量 1024

    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t)
    {
        producers.emplace_back([&bus] {
            for (int i = 0; i < kPerThread; ++i)
            {
                Msg m;
                m.head.msgType = MsgType::MSGTYPE_CONN_DATA;
                m.head.seq = (uint64_t)i;
                bus.sendToWorker(0, std::move(m));
            }
        });
    }
    for (auto& th : producers)
        th.join();

    int count = 0;
    Msg out;
    while (bus.tryPopWorker(0, out))
        ++count;
    CHECK_EQ(count, kThreads * kPerThread);
}

// 背压语义：队列写满时 try_push 必须返回 false（由调用方决定策略），
// 而不是无限增长把内存吃光
TEST(MsgBusBackpressure)
{
    MsgBus bus(1);

    int accepted = 0;
    for (int i = 0; i < 5000; ++i)
    {
        Msg m;
        m.head.msgType = MsgType::MSGTYPE_CONN_DATA;
        if (!bus.sendToWorker(0, std::move(m)))
            break;
        ++accepted;
    }

    // 必须在一万条以内就拒绝（否则说明没有容量上限）
    CHECK(accepted > 0);
    CHECK(accepted < 5000);
}
