#ifndef CLEARMOON_TIMER_TIMERTHREAD_H
#define CLEARMOON_TIMER_TIMERTHREAD_H

#include "common/msg.h"      // TimerOp / Msg / MsgType / ThreadId
#include "common/msgBus.h"
#include "common/queue.h"
#include "common/ITimerService.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Timer
{
// 进程内唯一 timerId 生成器，任意线程可调用。
// 定时器线程不分配 ID(否则 addTimer 需要等返回值，就退化成同步调用)，
// 调用方取 ID 自己保存，cancel 时传同一个值。
inline uint64_t NextTimerId()
{
    static std::atomic<uint64_t> s_next{1};
    return s_next.fetch_add(1, std::memory_order_relaxed);
}
} // namespace Timer

/**
 * @brief 256 槽时间轮定时器线程（单线程 + MPSC 命令队列）
 *
 * 结构：
 *   逻辑线程(任意多个) --TimerOp--> cmd_queue_(MPSC) --单消费者--> 定时器线程 --> 时间轮
 *   到期 -> 封装 MSG_TIMER_FIRE -> MsgBus::sendToWorker(ownerWorkerId) -> 回到原逻辑线程
 *
 * 为什么用"命令入队"而不是让定时器线程去 MsgBus 里取定时器消息：
 *   1. TimerOp 和业务 Msg 是两种完全不同的东西，混进一条总线会让 Msg 格式膨胀；
 *   2. 定时器线程只消费自己这条队列，时间轮状态(槽位/存活表)线程私有，全程无锁；
 *   3. 逻辑线程 addTimer/cancel 只做"入队 + 唤醒"，不阻塞、不持锁。
 *
 * TimerOp 字段约定(定义在 common/msg.h)：
 *   kind           ADD / CANCEL；CANCEL 只用 timerId
 *   timerId        调用方用 Timer::NextTimerId() 取，单定时器线程内唯一
 *   ownerWorkerId  ADD 必填，到期后 MSG_TIMER_FIRE 回投哪个逻辑线程
 *   intervalMs     超时时间；repeat=true 时同时作为重复周期
 *   repeat         是否周期触发
 *   module/method  到期后路由到 Lua 哪个模块/方法
 *   playerId/session/seq  到期后原样透传
 *
 * 关键保证：
 *   - 同一 Actor 的定时回调与它的其它消息一定落在同一逻辑线程上(ownerWorkerId)，
 *     不会出现"移动消息和定时器回调并发改同一个玩家"的情况；
 *   - 重复触发不漏 tick 计数、不因取消而内存泄漏：取消只从存活表移除，
 *     槽位里的 id 变成僵尸，扫到该槽时惰性剔除，在途僵尸数量有界；
 *   - 定时器回调到达逻辑线程后，逻辑层必须再校验一次 Actor 是否还活着，
 *     TimerOp 里的 playerId/session 是发起时刻的快照，不能当作"现在还存在"。
 */
class TimerThread : public ITimerService
{
public:
    static constexpr uint32_t kWheelSlots = 256;        // 时间轮槽数
    static constexpr uint64_t kMinTickMs = 1;           // tick 下限
    static constexpr uint64_t kMaxTickMs = 1000;        // tick 上限，避免精度过低
    static constexpr int64_t kMaxCatchupTicks = 5;      // 单次最多推进的 tick 数
    static constexpr size_t kMaxCommandsPerTick = 1024; // 单轮最多消费命令数，避免饿死触发
    static constexpr size_t kMaxPendingCommands = 1 << 16;

    // tickMs: 时间轮推进步长，到期精度即 tickMs(默认 50ms)
    // wheelSlots: 槽数(必须 > 1)，覆盖范围 = wheelSlots × tickMs
    TimerThread(MsgBus& bus,
                uint64_t tickMs = 50,
                uint32_t wheelSlots = kWheelSlots);
    ~TimerThread();

    TimerThread(const TimerThread&) = delete;
    TimerThread& operator=(const TimerThread&) = delete;
    TimerThread(TimerThread&&) = delete;
    TimerThread& operator=(TimerThread&&) = delete;

    bool start();
    void stop(); // 置停止标志 + 唤醒；线程退出前清空全部在途定时器(不触发回调)
    void join(); // 阻塞等待线程结束

    // ---- 生产者侧：任意线程可调用，非阻塞 ----
    // 入队成功返回 true；已停止或队列积压超过 kMaxPendingCommands 返回 false
    bool addTimer(const TimerOp& op);
    bool cancel(uint64_t timerId);

    // ---- 状态查询(监控用) ----
    bool running() const { return running_.load(std::memory_order_acquire); }
    uint64_t tickMs() const { return tick_ms_; }
    // 时间轮已扫描过的轮面毫秒数(= 已推进 tick 数 × tickMs)。
    // 注意它是"时间轮进度"而非墙钟：定时器线程跟不上时会落后于真实流逝时间，
    // 落后本身即是需要告警的信号(说明 tick 太小或单轮负载过重)。
    uint64_t nowMs() const { return now_ms_.load(std::memory_order_relaxed); }
    uint64_t firedCount() const { return fired_.load(std::memory_order_relaxed); }
    size_t liveCount() const { return live_count_.load(std::memory_order_relaxed); }
    size_t pendingCommands() const { return cmd_queue_.current_size(); }

private:
    // 与 TimerThread.cc 中 run() 使用的时间源保持一致
    using clock = std::chrono::steady_clock;

    // 时间轮内的一个定时器实例(内部 id 与调用方 id 分离)
    struct TimerItem
    {
        uint64_t internalId = 0;
        uint64_t userTimerId = 0;
        uint64_t ownerWorkerId = 0;
        uint64_t intervalMs = 0;
        bool repeat = false;

        uint16_t module = 0;
        uint16_t method = 0;
        uint64_t playerId = 0;
        uint64_t session = 0;
        uint64_t seq = 0;
    };

    struct LiveTimer
    {
        uint64_t leftRound = 0; // 还需绕的圈数
        TimerItem item;
    };

    void run();
    void wakeup();     // 通知定时器线程：命令队列有新数据
    void processOps(const clock::time_point& deadline); // 消费命令队列(仅定时器线程)
    void doAdd(const TimerOp& op);
    void doCancel(uint64_t userTimerId);
    void insert(TimerItem&& item, uint64_t firstDelayMs);
    void collect(uint32_t slot, std::vector<TimerItem>& firedOut);
    void fire(const TimerItem& item);
    void clearAll();

    MsgBus* m_bus_ = nullptr;

    uint64_t tick_ms_ = 50;
    uint32_t wheel_slots_ = kWheelSlots;

    // slots_[slot] 存内部 id(uint64)，避免槽位间搬移导致引用失效
    std::vector<std::vector<uint64_t>> slots_;
    // 内部 id -> 存活定时器
    std::unordered_map<uint64_t, LiveTimer> live_;
    // 调用方 id -> 该 id 当前存活的内部 id(正常 1 个；重复 ADD 时多个)
    std::unordered_map<uint64_t, std::vector<uint64_t>> byUser_;

    uint64_t next_internal_id_ = 1;
    uint32_t cur_slot_ = 0;
    std::atomic<uint64_t> now_ms_{0}; // 时间轮累计推进毫秒(仅定时器线程写)

    // 时间轮位置(仅定时器线程使用)。
    // 位置由"真实时钟 - start"推导，而不是"每轮 +1"：这样在平台 wait 粒度粗于 tick 时，时间轮也不会跑得比真实时间快（定时器只会晚，不会提前）。
    int64_t advanced_ticks_ = 0; // 已真正推进(扫描过)的 tick 数
    int64_t skipped_ticks_ = 0;  // 因限流暂缓推进、后续要补上的 tick 数

    // 命令队列：多生产者(任意逻辑线程) -> 单消费者(定时器线程)
    MsgQueue<TimerOp> cmd_queue_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> fired_{0};
    std::atomic<size_t> live_count_{0};

    std::thread thread_;

    // 仅用于等待/唤醒，不保护任何数据；定时器状态全在定时器线程内部
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
};

#endif