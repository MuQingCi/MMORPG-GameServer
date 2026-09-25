#include "timer/timerQueue.h"
#include "event/eventLoop.h"
#include "timer/timer.h"
#include "timer/timerId.h"
#include "common/timestamp.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

/**
 * @brief 创建 timerfd
 */
int createTimerfd()
{
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(timerfd >= 0);
    return timerfd;
}

/**
 * @brief 计算当前时间（微秒）到到期时间的间隔
 * @return timespec 结构体（秒 + 纳秒）
 */
struct timespec howMuchTimeFromNow(Timestamp when)
{
    int64_t micro = when.getMicroSecond() - Timestamp::now().getMicroSecond();

    // 如果已经过期，立即触发
    if (micro < 100)
    {
        micro = 100;
    }

    struct timespec ts;
    ts.tv_sec  = static_cast<time_t>(micro / 1000000);
    ts.tv_nsec = static_cast<long>((micro % 1000000) * 1000);
    return ts;
}

//向上取整到 tick 边界（微秒）：同一 tick 内的定时器共享同一臂装时间
int64_t alignUpTickUs(int64_t us)
{
    return (us + TimerQueue::kTickUs - 1) / TimerQueue::kTickUs * TimerQueue::kTickUs;
}

} // anonymous namespace

// ==================== TimerQueue 实现 ====================

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(std::make_unique<Channel>(loop_, timerfd_)),
      timers_()
{
    timerfdChannel_->setReadCallback([this] { handleRead(); });
    timerfdChannel_->enableReading();
}

TimerQueue::~TimerQueue()
{
    timerfdChannel_->disableAll();
    timerfdChannel_->remove();
    ::close(timerfd_);

    // 释放所有 Timer
    for (const auto& entry : timers_)
    {
        delete entry.timer_;
    }
    timers_.clear();
    activeTimers_.clear();
}

TimerId TimerQueue::addTimer(TimerCallback cb, Timestamp when, double interval)
{
    auto* timer = new Timer(std::move(cb), when, interval);
    
    TimerId timerId(timer, timer->sequence());

    // 直接调用 addTimerInLoop，EventLoop 里保证 runInLoop 是线程安全的
    loop_->runInLoop([this, timer]() { addTimerInLoop(timer); });

    return timerId;
}

void TimerQueue::addTimerInLoop(Timer* timer)
{
    assert(loop_->isInThread());

    bool earliestChanged = false;
    Timestamp expiration = timer->expiration();

    // 如果 timer 是最早到期的，需要重置 timerfd
    if (timers_.empty() || expiration < timers_.begin()->expiration_)
    {
        earliestChanged = true;
    }

    timers_.insert({expiration, timer});
    activeTimers_.insert({timer->sequence(), timer});

    if (earliestChanged)
    {
        armTimerFd(expiration);
    }
}

void TimerQueue::cancel(TimerId timerId)
{
    if(loop_->isInThread())
    {
        cancelInLoop(timerId);
    }
    else {
        loop_->runInLoop([this, timerId]{ cancelInLoop(timerId); });
    }
}

void TimerQueue::cancelInLoop(TimerId timerId)
{
    loop_->assertInLoopThread();

    ActiveTimer key = {timerId.sequence_, timerId.timer_};

    //在活动定时器集合中查找目标定时器
    auto it = activeTimers_.find(key);
    //存在于活动定时器集合则直接释放
    if(it != activeTimers_.end())
    {
        Timer* timer = it->second;
        timers_.erase({timer->expiration(), timer});
        activeTimers_.erase({timer->sequence(), timer});
        delete timer;
    }
    //如果目标定时器不存在于activeTimers_且正在执行处理定时器回调阶段
    else if (handleTimer_) {
        cancelligTimer_.insert({timerId.sequence_, timerId.timer_});
    }
    else {
        //自然消亡的定时器
    }
}

void TimerQueue::handleRead()
{
    assert(loop_->isInThread());

    // 本次臂装已被消费；置 0 使后续新增定时器必然重新臂装（防止"过期即加"漏臂）
    armedUs_ = 0;

    // 读取 timerfd 的到期标记（必须读，否则 epoll 会重复触发）
    uint64_t exp = 0;
    ssize_t ret = ::read(timerfd_, &exp, sizeof exp);
    if (ret != sizeof exp)
    {
        // 非阻塞模式下可能出现 EAGAIN，忽略
    }

    Timestamp now(Timestamp::now());

    handleTimer_ = true;

    // 获取所有到期的定时器
    std::vector<Entry> expired = getExpired(now);

    // 执行回调
    for (const auto& entry : expired)
    {
        if(entry.timer_->isCancelled()) continue;
        entry.timer_->run();
    }

    handleTimer_ = false;

    // 重置重复定时器
    reset(expired, now);
    cancelligTimer_.clear();
}

/**
 * @brief 1.先通过lower_bound找到第一个到期绝对时间大于当前时间的定时器迭代器
          2.再把到期定时器复制到新容器，随后把到期定时器从原容器中删除
          该函数运行结束后，timers_中只剩下未到期的定时器
 * 
 * @param now 
 * @return std::vector<TimerQueue::Entry> 
 */
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
    assert(loop_->isInThread());

    // 哨兵：查找第一个到期时间 > now 的定时器
    Entry sentry = {now, reinterpret_cast<Timer*>(UINTPTR_MAX)};

    auto it = timers_.lower_bound(sentry);
    assert(it == timers_.end() || now < it->expiration_);

    //将到期定时器从timers_和activeTimer_中移出
    for(auto Entry = timers_.begin(); Entry != it; Entry++)
    {
        activeTimers_.erase({Entry->timer_->sequence(), Entry->timer_});
    }

    //先把到期的定时器复制到新容器
    std::vector<Entry> expired;
    std::copy(timers_.begin(), it, std::back_inserter(expired));

    //随后把到期的定时器在timers_中删除
    timers_.erase(timers_.begin(), it);

    return expired;
}


void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
    assert(loop_->isInThread());

    for (const auto& entry : expired)
    {
        //判断当前定时器是否存在于cancellingTimer中，存在则直接释放
        ActiveTimer key = {entry.timer_->sequence(), entry.timer_};

        if(cancelligTimer_.count(key))
        {
            delete entry.timer_;
            continue;
        }

        //当前定时器是否被标记为删除，是则直接释放
        if(entry.timer_->isCancelled())
        {
            delete entry.timer_;
            continue;
        }

        // 重复定时器重新加入
        if (entry.timer_->repeat())
        {
            auto* timer = entry.timer_;
            timer->restart(now);
            timers_.insert({timer->expiration(), timer});
            activeTimers_.insert({timer->sequence(), timer});
        }
        else
        {
            // 一次性定时器，释放
            delete entry.timer_;
        }
    }

    // 如果还有定时器剩余，重置 timerfd 到下一个最早到期时间
    if (!timers_.empty())
    {
        armTimerFd(timers_.begin()->expiration_);
    }
}

//tick 对齐 + 去重重臂：同一 tick 内的定时器共享一次 timerfd_settime
void TimerQueue::armTimerFd(Timestamp expiration)
{
    int64_t aligned = alignUpTickUs(expiration.getMicroSecond());
    if(armedUs_ != 0 && armedUs_ == aligned)
        return;
    armedUs_ = aligned;
    resetTimerFd(timerfd_, Timestamp(aligned));
}

void TimerQueue::resetTimerFd(int timerfd, Timestamp expiration)
{
    struct itimerspec newValue;
    struct itimerspec oldValue;
    std::memset(&newValue, 0, sizeof newValue);
    std::memset(&oldValue, 0, sizeof oldValue);

    newValue.it_value = howMuchTimeFromNow(expiration);
    int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
    if (ret < 0)
    {
        // 错误忽略
    }
}