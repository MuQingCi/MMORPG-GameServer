#include "TimerThread.h"
#include "log/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{
// MsgBus::sendToWorker 目前返回 void，拿不到"逻辑队列是否已满"的结果。
// 若后续改成返回 bool，把这里切到 kLogFireFailure 分支即可。
constexpr bool kLogFireFailure = false;

void logDrop(const char* what, uint64_t id)
{
    LOG_WARNING<< what << "failed(queue full/stopped), timerId=" << id;
}
} // namespace

TimerThread::TimerThread(MsgBus& bus, uint64_t tickMs, uint32_t wheelSlots)
                        :m_bus_(&bus),
                         tick_ms_(std::clamp(tickMs, kMinTickMs, kMaxTickMs)),
                         wheel_slots_(wheelSlots > 1 ? wheelSlots : kWheelSlots),
                         slots_(wheelSlots > 1 ? wheelSlots : kWheelSlots)
{
    if (tickMs != tick_ms_)
        LOG_WARNING<<"[TimerThread] tickMs=" << tickMs << ",clamped to " << tick_ms_;
}

TimerThread::~TimerThread()
{
    stop();
    join();
}

bool TimerThread::start()
{
    if (running_.load(std::memory_order_acquire))
        return true;

    // 支持 stop -> start 复用：此时线程已 join，无并发访问，可安全重置状态
    stop_.store(false, std::memory_order_release);
    clearAll();
    now_ms_.store(0, std::memory_order_relaxed);
    cur_slot_ = 0;

    running_.store(true, std::memory_order_release);

    try
    {
        thread_ = std::thread([this] { run(); });
    }
    catch (...)
    {
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void TimerThread::stop()
{
    // 幂等：先置停止标志(release)，再唤醒可能正 wait 的定时器线程
    stop_.store(true, std::memory_order_release);
    wakeup();
}

void TimerThread::join()
{
    if (thread_.joinable())
        thread_.join();
}

bool TimerThread::addTimer(const TimerOp& op)
{
    if (stop_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire))
        return false;

    if (op.timerId == 0)
    {
        LOG_INFO<<"[TimerThread] addTimer rejected: timerId == 0\n";
        return false;
    }

    // 背压：积压过多说明定时器线程已被拖慢，拒绝而不是无限堆积
    if (cmd_queue_.current_size() >= kMaxPendingCommands)
    {
        logDrop("addTimer", op.timerId);
        return false;
    }

    if (!cmd_queue_.try_push(TimerOp(op))) // 拷贝入队，调用方的结构体可复用
    {
        logDrop("addTimer", op.timerId);
        return false;
    }

    wakeup();
    return true;
}

bool TimerThread::cancel(uint64_t timerId)
{
    if (stop_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire))
        return false;

    if (timerId == 0)
        return false;

    TimerOp op{};
    op.kind = TimerOp::CANCEL;
    op.timerId = timerId;

    if (cmd_queue_.current_size() >= kMaxPendingCommands)
    {
        logDrop("cancel", timerId);
        return false;
    }

    if (!cmd_queue_.try_push(std::move(op)))
    {
        logDrop("cancel", timerId);
        return false;
    }

    wakeup();
    return true;
}

void TimerThread::wakeup()
{
    // 只做通知，不保护数据。消费者进入 wait 前会检查命令队列，
    // 因此"通知早于 wait"不会导致永眠。
    wait_cv_.notify_one();
}

void TimerThread::run()
{
    using clock = std::chrono::steady_clock;

    std::vector<TimerItem> fired;
    fired.reserve(256);

    using Ms = std::chrono::milliseconds;
    const Ms tickDur(static_cast<int64_t>(tick_ms_));
    const int64_t tickMs = static_cast<int64_t>(tick_ms_);

    const auto start = clock::now();
    auto nextTick = start + tickDur;

    while (!stop_.load(std::memory_order_acquire))
    {
        // 1. 消费命令队列(ADD/CANCEL)
        //    用同一份时间预算约束：不能因为命令洪水在一个 tick 内反复循环，
        //    否则每轮只推进一个 tick 会让时间轮跑得比真实时间快。
        //    没消费完的命令留在队列里，下一轮继续；wait 的谓词会立刻返回。
        processOps(nextTick); // deadline = 本 tick 结束时刻

        if (stop_.load(std::memory_order_acquire))
            break;

        // 2. 推进时间轮。
        //
        //    位置由真实时钟推导：目标位置 = floor((now - start) / tick)，
        //    本轮只推进 (目标位置 - 已推进位置) 个槽。这是"时钟驱动"而不是
        //    "计数器累加"，因此时间轮累计推进的轮面时间永远不会超过真实流逝时间
        //    —— 定时器只会晚触发，绝不会提前触发。
        //
        //    为什么必须这样：平台 wait 粒度可能远粗于 tick(例如 Windows 默认
        //    ~15.6ms，而 tick 只有 5ms)。若按"每轮 +1"或"落后就补满"来推进，
        //    每 15.6ms 就会推进好几个 tick，时间轮越算越快，所有定时器提前触发。
        //
        //    单轮最多推进 kMaxCatchupTicks 个槽。落后太多时只按上限推进，
        //    跳过的部分累加到 skipped_ticks_ 里，在后续轮次的目标位置中扣除，
        //    这样既不会长时间占满 CPU，也不会丢掉任何槽位；跳过的槽里到期的
        //    定时器会在补到该槽时触发(偏晚，但绝不提前，也绝不丢)。
        const auto now = clock::now();

        const int64_t elapsedTicks =
            std::chrono::duration_cast<Ms>(now - start).count() / tickMs;

        int64_t advanceTicks = elapsedTicks - skipped_ticks_ -
                               static_cast<int64_t>(advanced_ticks_);
        if (advanceTicks < 0)
            advanceTicks = 0;
        if (advanceTicks > kMaxCatchupTicks)
        {
            skipped_ticks_ += advanceTicks - kMaxCatchupTicks;
            advanceTicks = kMaxCatchupTicks;
        }

        for (int64_t i = 0; i < advanceTicks; ++i)
        {
            if (stop_.load(std::memory_order_acquire))
                break;

            cur_slot_ = (cur_slot_ + 1) % wheel_slots_;
            now_ms_.fetch_add(tick_ms_, std::memory_order_relaxed);
            ++advanced_ticks_;

            // 先摘出到期项，再统一触发(触发过程中不修改槽位)
            fired.clear();
            collect(cur_slot_, fired);
            for (auto& it : fired)
                fire(it);
        }

        live_count_.store(live_.size(), std::memory_order_relaxed);

        // 3. 睡到下一个 tick 时刻。
        //    已经落后时把 nextTick 拉到 now(立即进入下一轮继续追赶)，
        //    但绝不拉到 now 之后 —— 那样会让时间轮永远追不上真实时钟。
        if (nextTick < now)
            nextTick = now;

        int64_t sleepMs = std::chrono::duration_cast<Ms>(nextTick - now).count();
        if (sleepMs < 0)
            sleepMs = 0;
        nextTick += tickDur;

        std::unique_lock<std::mutex> lk(wait_mu_);
        wait_cv_.wait_for(lk, Ms(sleepMs), [this] {
            return stop_.load(std::memory_order_acquire) ||
                   cmd_queue_.current_size() > 0;
        });
    }

    clearAll();
    running_.store(false, std::memory_order_release);
    LOG_INFO<<"timer thread stop";
}

void TimerThread::processOps(const clock::time_point& deadline)
{
    TimerOp op;
    size_t n = 0;

    // 双约束：既限量(kMaxCommandsPerTick，防止单轮占用过久)，
    // 也限时(deadline，防止时间轮在一个 tick 内反复循环而跑快)。
    // 没消费完的命令留在队列里，下一轮继续。
    while (n < kMaxCommandsPerTick && cmd_queue_.try_pop(op))
    {
        if (op.kind == TimerOp::ADD)
            doAdd(op);
        else
            doCancel(op.timerId);
        ++n;

        if (clock::now() >= deadline)
            break;
    }
}

void TimerThread::doAdd(const TimerOp& op)
{
    TimerItem item;
    item.internalId = next_internal_id_++;
    item.userTimerId = op.timerId;
    item.ownerWorkerId = op.ownerWorkerId;
    item.repeat = op.repeat;
    item.Module = op.Module;
    item.Method = op.Method;
    item.playerId = op.playerId;
    item.session = op.session;
    item.seq = op.seq;

    // intervalMs 既是超时时间，也是 repeat 时的周期；下限一个 tick，
    // 避免 0 周期导致每个 tick 都触发。
    uint64_t interval = op.intervalMs;
    if (interval < tick_ms_)
        interval = tick_ms_;
    item.intervalMs = interval;

    const uint64_t internalId = item.internalId;
    const uint64_t userTimerId = item.userTimerId;

    // 相同 timerId 的旧定时器不做隐式取消(重复 ADD 会各自触发)，仅登记索引
    byUser_[userTimerId].push_back(internalId);

    insert(std::move(item), interval);

    if (live_.find(internalId) == live_.end())
        std::fprintf(stderr, "[TimerThread] insert failed userTimerId=%llu\n",
                     static_cast<unsigned long long>(userTimerId));
}

void TimerThread::doCancel(uint64_t userTimerId)
{
    auto it = byUser_.find(userTimerId);
    if (it == byUser_.end())
        return; // 不存在或已触发：幂等忽略

    std::vector<uint64_t> ids;
    ids.swap(it->second);
    byUser_.erase(it);

    for (uint64_t internalId : ids)
    {
        // 只从 live_ 移除：槽位里的内部 id 变成"僵尸"，
        // 等时间轮扫到该槽位时由 collect() 惰性剔除。
        // 最坏情况是它还要绕 leftRound 圈才被扫到，成本只是一个 uint64。
        live_.erase(internalId);
    }
}

void TimerThread::insert(TimerItem&& item, uint64_t firstDelayMs)
{
    if (slots_.size() != wheel_slots_)
        slots_.resize(wheel_slots_);

    // 至少延后一个 tick，消除"当前槽即刻触发"的歧义
    uint64_t delay = firstDelayMs;
    if (delay < tick_ms_)
        delay = tick_ms_;

    const uint64_t totalTicks = (delay + tick_ms_ - 1) / tick_ms_; // 向上取整
    const uint64_t slotSpan = wheel_slots_;

    const uint64_t leftRound = totalTicks / slotSpan;
    const uint64_t inSlot = totalTicks % slotSpan;
    const uint32_t slot = static_cast<uint32_t>((cur_slot_ + inSlot) % slotSpan);

    const uint64_t internalId = item.internalId;

    LiveTimer lt;
    lt.leftRound = leftRound;
    lt.item = std::move(item);

    live_[internalId] = std::move(lt);
    slots_[slot].push_back(internalId);
}

void TimerThread::collect(uint32_t slot, std::vector<TimerItem>& firedOut)
{
    if (slot >= slots_.size())
        return;

    auto& bucket = slots_[slot];
    if (bucket.empty())
        return;

    std::vector<uint64_t> keep;
    keep.reserve(bucket.size());

    for (uint64_t internalId : bucket)
    {
        auto it = live_.find(internalId);
        if (it == live_.end())
            continue; // 僵尸项：已取消/已触发/已被覆盖，惰性剔除

        if (it->second.leftRound > 0)
        {
            --it->second.leftRound;
            keep.push_back(internalId);
            continue;
        }

        TimerItem ti = std::move(it->second.item);
        live_.erase(it);

        firedOut.push_back(std::move(ti));
    }

    bucket.swap(keep);
}

void TimerThread::fire(const TimerItem& item)
{
    fired_.fetch_add(1, std::memory_order_relaxed);

    // 1. 周期定时器：重新挂回时间轮(以 intervalMs 作为首次延迟)
    if (item.repeat)
    {
        TimerItem next = item;
        next.internalId = next_internal_id_++;
        const uint64_t interval = item.intervalMs;
        const uint64_t newId = next.internalId;

        byUser_[item.userTimerId].push_back(newId);

        insert(std::move(next), interval);
    }
    else
    {
        // 一次性定时器：清掉用户索引
        auto uit = byUser_.find(item.userTimerId);
        if (uit != byUser_.end())
        {
            auto& ids = uit->second;
            ids.erase(std::remove(ids.begin(), ids.end(), item.internalId), ids.end());
            if (ids.empty())
                byUser_.erase(uit);
        }
    }

    // 2. //TODO 到期事件回投 ownerWorkerId 对应的逻辑线程。
    //    定时器回调可重建(幂等)，绝不能阻塞时间轮线程；
    //    逻辑层收到后必须重新校验 Actor 是否还活着。
    Msg m;
    m.head.msgType     = MsgType::MSGTYPE_TIMER_FIRE;
    m.head.Module      = item.Module;
    m.head.Method      = item.Method;
    m.head.seq         = item.seq;
    m.head.session     = static_cast<uint32_t>(item.session);
    m.head.playerId    = item.playerId;
    m.head.srcWorkerId = static_cast<uint32_t>(item.ownerWorkerId);
    m.head.ctx         = item.userTimerId;
    m.body.clear();

    m_bus_->sendToWorker(static_cast<uint32_t>(item.ownerWorkerId), m);

    if (kLogFireFailure)
    {
        LOG_WARNING<<"[TimerThread] TIMER_FIRE timerId= "<<item.userTimerId
                   <<", worker="<<item.ownerWorkerId
                   <<", Module="<<item.Module
                   <<", Method=" <<item.Method;
    }
}

void TimerThread::clearAll()
{
    // 只清容器，不触发任何回调(退出/重启路径)
    for (auto& bucket : slots_)
        bucket.clear();
    live_.clear();
    byUser_.clear();
    next_internal_id_ = 1;
    live_count_.store(0, std::memory_order_relaxed);
    advanced_ticks_ = 0;
    skipped_ticks_ = 0;
}