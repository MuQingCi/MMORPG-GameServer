#ifndef CLEARMOON_COMMON_MSGBUS_H
#define CLEARMOON_COMMON_MSGBUS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct Msg;

template<typename T>
class MsgQueue;

/**
 * @brief 场景服内部全局消息总线
 *
 * 结构（与设计文档 3.2 对应）：
     -------------                                  ---------------------
 *   | 网络线程   |                                  | 逻辑线程 0 队列   |
 *   | 定时器线程 |---> worker_queues_（每线程一条）--| 逻辑线程 1 队列   |
 *   | DB 线程   |                                  | ...              |
 *   | 逻辑线程  |                                   ------------------- 
     ------------   ---> net_queue_（网络线程单消费者）
 *                  ---> mysql_queue_ / redis_queue_（两条 DB 通道，各自单消费者）
 *
 * 三条不变式（改动本文件前必须确认）：
 *   1. 每条队列都是 MPSC：多生产者任意线程可 push，单消费者只能由其所属线程 pop；
 *   2. 线程归属是纯函数 hash(id) % N，不在总线上维护任何 id -> worker 的表；
 *   3. 跨线程只投消息，不共享对象（DB 结果回投到 srcWorkerId，由该线程自己进 Lua）。
 *
 * 为什么 MySQL 与 Redis 必须分两条通道：
 *   Redis 是亚毫秒级、MySQL 是毫秒到几十毫秒级。共用一个队列时，
 *   一条慢 SQL 会把后面所有 Redis 读全部堵死（建议书 F2 / 设计文档 3.5）。
 */
class MsgBus
{
public:
    using ActorId  = uint64_t;
    using ModuleId = uint32_t;
    using WorkerId = uint32_t;
    using PlayerId = uint64_t;

    explicit MsgBus(size_t numWorkers);
    ~MsgBus();

    MsgBus(const MsgBus&) = delete;
    MsgBus& operator=(const MsgBus&) = delete;
    MsgBus(MsgBus&&) = delete;
    MsgBus& operator=(MsgBus&&) = delete;

    // ---------------- 逻辑线程路由 ----------------
    // Actor 归属：hash(actorId) % N。玩家即 Actor 时等价于 sendToWorkerByPlayerId
    bool sendToActor(ActorId id, Msg m);
    // 无状态模块消息：同一模块的消息串行落到同一线程（避免模块内状态竞争）
    bool sendToModule(ModuleId mid, Msg m);
    bool sendToWorkerByPlayerId(PlayerId pid, Msg m);
    // 精准投递（结果回投 / 会话亲和 / 定时器到期）
    bool sendToWorker(WorkerId wid, Msg m);
    // 广播到全部逻辑线程（热更、全服公告等），msgs 必须可拷贝
    void broadcastToLogic(const Msg& m);

    // ---------------- 辅助线程投递 ----------------
    bool sendToNet(Msg m);
    // 按 msgType 自动选择 DB 通道（MSGTYPE_DB_TASK_MYSQL / MSGTYPE_DB_TASK_REDIS）
    bool sendToDb(Msg m);

    // ---------------- 消费侧（只能在对应线程调用）----------------
    bool tryPopWorker(WorkerId wid, Msg& out);
    bool waitPopWorker(WorkerId wid, Msg& out, std::chrono::milliseconds timeoutMs);

    bool tryPopNet(Msg& out);
    bool waitPopNet(Msg& out, std::chrono::milliseconds timeoutMs);

    bool tryPopDbMySql(Msg& out);
    bool waitPopDbMySql(Msg& out, std::chrono::milliseconds timeoutMs);

    bool tryPopDbRedis(Msg& out);
    bool waitPopDbRedis(Msg& out, std::chrono::milliseconds timeoutMs);

    // ---------------- 唤醒回调 ----------------
    // 消费者线程启动前注册一次；生产者入队后调用它把消费者从 epoll/阻塞中叫醒。
    // 为什么要它：网络线程不能靠"超时轮询"取消息（无谓唤醒 + 延迟），
    // 而生产者（逻辑线程）也不该直接持有网络线程的 eventfd。
    using Wakeup = std::function<void()>;
    void setNetWakeup(Wakeup cb);

    // ---------------- 负载信息 ----------------
    size_t numWorker() const { return worker_queues_.size(); }
    // 队列积压深度（选线程的唯一依据：accept 时看不到"连接数"这种全局量）
    size_t depth(WorkerId wid) const;
    // 选一个最空闲的逻辑线程，同分用 tieBreak 哈希打散，避免羊群效应
    WorkerId leastLoadedWorker(uint64_t tieBreak) const;

private:
    std::vector<std::unique_ptr<MsgQueue<Msg>>> worker_queues_;
    std::unique_ptr<MsgQueue<Msg>> net_queue_;
    std::unique_ptr<MsgQueue<Msg>> mysql_queue_;
    std::unique_ptr<MsgQueue<Msg>> redis_queue_;

    Wakeup net_wakeup_;
};

#endif
