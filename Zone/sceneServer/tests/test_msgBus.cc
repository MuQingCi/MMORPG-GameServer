#include "common/hash.h"
#include "common/msg.h"
#include "common/msgBus.h"
#include "metrics/queueDiagnosticsJson.h"
#include "test_util.h"

#include <set>
#include <thread>
#include <vector>

TEST(QueueDiagnosticsSnapshotSeparatesCollectionAndSerialization)
{
    QueueDiagnostics diagnostic(1);
    diagnostic.admission(0, 0);
    diagnostic.dequeued(0, 0, 10, 30);
    diagnostic.event(0, "test", 10, 30, 7);
    const auto light = diagnostic.collect(100, {2}, 3, false);
    CHECK(light.events.empty());
    CHECK(light.waits.empty());
    const auto full = diagnostic.collect(110, {1}, 0);
    CHECK_EQ(full.events.size(), 1u);
    CHECK_EQ(full.waits[0][0].sumUs, 20u);
    CHECK_EQ(full.admissions[0][1], 1u);
    const auto json = serializeQueueDiagnostics(full);
    CHECK_EQ(json, serializeQueueDiagnostics(full));
    CHECK(json.find("\"monotonicUs\":110") != std::string::npos);
    const auto next = diagnostic.collect(120, {0}, 0);
    CHECK(next.events.empty());
    CHECK_EQ(next.waits[0][0].count, 1u);
    CHECK_EQ(next.admissions[0][0], 1u);
}

// 归属必须是**纯函数且稳定**：同一 playerId 在任何时候都落到同一线程
TEST(QueueDiagnosticsBoundariesAndRequeue)
{
    CHECK_EQ(QueueDiagnostics::bucket(100),0u);
    CHECK_EQ(QueueDiagnostics::bucket(101),1u);
    CHECK_EQ(QueueDiagnostics::bucket(100001),9u);
    QueueDiagnostics d(1);
    d.dequeued(0,0,100,200); d.dequeued(0,0,0,200); d.dequeued(0,0,300,200);
    CHECK_EQ(d.wait(0,0).count,1u); CHECK_EQ(d.wait(0,0).missing,2u);
    MsgBus bus(2); bus.enableDiagnostics();
    Msg m; m.head.msgType=MSGTYPE_CONN_DATA; m.head.hasClientBinding=true;
    CHECK(IsAccepted(bus.sendToWorker(0,m)));
    Msg out; CHECK(bus.tryPopWorker(0,out));
    CHECK(out.diagnosticEnqueuedUs>0);
    const auto previous=out.diagnosticEnqueuedUs;
    out.diagnosticEnqueuedUs=1;
    CHECK(IsAccepted(bus.sendToWorker(0,out))); CHECK(bus.tryPopWorker(0,out));
    CHECK(out.diagnosticEnqueuedUs>=previous);
    CHECK_EQ(bus.enqueuedBusinessCount(0),2u);
    CHECK_EQ(bus.diagnostics()->wait(0,0).count,2u);
    CHECK_EQ(bus.broadcastToLogic(m).size(),2u);
    CHECK(bus.tryPopWorker(0,out)); CHECK(bus.tryPopWorker(1,out));
    CHECK_EQ(bus.diagnostics()->wait(1,0).count,1u);
    CHECK(!IsAccepted(bus.sendToWorker(99,m)));
    MsgBus disabled(1); CHECK(IsAccepted(disabled.sendToWorker(0,out)));
    CHECK(disabled.tryPopWorker(0,out)); CHECK_EQ(out.diagnosticEnqueuedUs,0u);
}

TEST(QueueDiagnosticsChannelAccounting)
{
    MsgBus bus(2); bus.enableDiagnostics();
    Msg m; m.head.msgType=MSGTYPE_DB_TASK_MYSQL;
    CHECK(IsAccepted(bus.sendToDb(m)));
    Msg out; CHECK(bus.tryPopDbMySql(out));
    CHECK_EQ(bus.diagnostics()->wait(3,0).count,1u); // workers, net, mysql
    CHECK(IsAccepted(bus.sendToNet(m))); CHECK(bus.tryPopNet(out));
    CHECK_EQ(bus.diagnostics()->wait(2,0).count,1u);
    CHECK_EQ(bus.diagnostics()->admissions(3)[0],1u);
    CHECK_EQ(bus.diagnostics()->admissions(3)[1],1u);
    m.head.msgType=MSGTYPE_DB_TASK_REDIS; m.head.dbShard=99;
    CHECK(!IsAccepted(bus.sendToDb(m)));
    CHECK_EQ(bus.diagnostics()->admissions(5)[3],1u); // invalid-target channel
    Msg load;
    load.head.msgType=MSGTYPE_DB_TASK_MYSQL;
    load.head.Method=Method::SYS_FLUSH;
    load.head.taskId=123;
    CHECK(IsAccepted(bus.sendToDb(load))); CHECK(bus.tryPopDbMySql(out));
    CHECK(bus.diagnostics()->json(1,{0,0},0).find("save_db_accepted")==std::string::npos);
    load.diagnosticSaveTask=true;
    CHECK(IsAccepted(bus.sendToDb(load))); CHECK(bus.tryPopDbMySql(out));
    CHECK(bus.diagnostics()->json(2,{0,0},0).find("save_db_accepted")!=std::string::npos);
}

TEST(QueueDiagnosticsConcurrentSnapshotAndBoundedEvents)
{
    QueueDiagnostics d(2);
    std::thread first([&] { for(int i=0;i<2000;++i) { d.dequeued(0,0,10,20); d.admission(0,0); } });
    std::thread second([&] { for(int i=0;i<2000;++i) d.dequeued(1,1,10,30); });
    for(int i=0;i<20;++i) CHECK(!d.json(100,{0,0},0).empty());
    first.join(); second.join();
    CHECK_EQ(d.wait(0,0).count,2000u); CHECK_EQ(d.wait(1,1).sumUs,40000u);
    CHECK_EQ(d.admissions(0)[0],2000u);
    for(int i=0;i<4100;++i) d.event(0,"test",1,2,i);
    const auto full=d.json(100,{0,0},0);
    CHECK(full.find("\"droppedEvents\":4")!=std::string::npos);
    CHECK(d.json(101,{0,0},0,false).find("\"waits\":[]")!=std::string::npos);
}

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
        CHECK(IsAccepted(bus.sendToWorkerByPlayerId(pid, m)));
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
    CHECK(IsAccepted(bus.sendToDb(mysqlTask)));

    Msg redisTask;
    redisTask.head.msgType = MsgType::MSGTYPE_DB_TASK_REDIS;
    redisTask.head.ctx = 22;
    CHECK(IsAccepted(bus.sendToDb(redisTask)));

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
        CHECK(IsAccepted(bus.sendToDb(m)));
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
    CHECK(!IsAccepted(bus.sendToDb(bad)));
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
                IsAccepted(bus.sendToWorker(0, std::move(m)));
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
        if (!IsAccepted(bus.sendToWorker(0, std::move(m))))
            break;
        ++accepted;
    }

    // 必须在一万条以内就拒绝（否则说明没有容量上限）
    CHECK(accepted > 0);
    CHECK(accepted < 5000);
}
