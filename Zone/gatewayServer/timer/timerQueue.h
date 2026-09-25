#ifndef CLEARMOON_GATEWAY_TIMER_TIMERQUEUE_H
#define CLEARMOON_GATEWAY_TIMER_TIMERQUEUE_H

#include "base/noncopy.h"
#include "event/channel.h"
#include "common/timestamp.h"
#include "timer/timerId.h"

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>        

class EventLoop;
class Timer;

/**
 * @brief 定时器队列
 *        内部使用 std::set 按到期时间管理所有定时器,
 *        使用一个 timerfd 来唤醒 epoll_wait.
 */
class TimerQueue : public noncopyable
{
public:
    using TimerCallback = std::function<void()>;

    //tick 粒度（微秒）：同一 tick 内所有定时器共享一次 timerfd 臂装，settim 频次由 O(每请求) 降到 O(每秒 tick 数)
    static constexpr int64_t kTickUs = 1000;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    /**
     * @brief 注册一个定时器（线程安全，可跨线程调用）
     * @param cb     回调
     * @param when   到期时间
     * @param interval 间隔，<=0 一次性，>0 重复
     * @return TimerId 可用于取消
     */
    TimerId addTimer(TimerCallback cb, Timestamp when, double interval);

    /**
     * @brief 取消定时器
     */
    void cancel(TimerId timerId);

private:
    // 按到期时间排序用
    struct Entry
    {
        Timestamp expiration_;
        Timer* timer_;

        // 先按 expiration 升序，再按 timer 地址升序（处理到期时间相同的场景）
        bool operator<(const Entry& rhs) const
        {
            if (expiration_.getMicroSecond() != rhs.expiration_.getMicroSecond())
                return expiration_.getMicroSecond() < rhs.expiration_.getMicroSecond();
            return timer_ < rhs.timer_;
        }
    };

using ActiveTimer = std::pair<uint64_t, Timer*>;
    // 插入一个新定时器（内部实现，必须在 EventLoop 线程调用）
    void addTimerInLoop(Timer* timer);

    //
    void cancelInLoop(TimerId timerId);

    // timerfd 可读时的回调
    void handleRead();

    // 获取所有到期的定时器
    std::vector<Entry> getExpired(Timestamp now);

    // 重置重复定时器（expired 中那些要重复的，重新插入）
    void reset(const std::vector<Entry>& expired, Timestamp now);

    // 将最早到期时间设置到 timerfd
    void resetTimerFd(int timerfd, Timestamp expiration);

    // tick 对齐 + 去重重臂：仅当对齐后到期时间与当前已臂装值不同才调用 timerfd_settime
    void armTimerFd(Timestamp expiration);

    EventLoop* loop_;
    const int timerfd_;                         // timerfd 文件描述符
    std::unique_ptr<Channel> timerfdChannel_;   // timerfd 对应的 Channel

    // 所有已注册的定时器（按到期时间排序）
    std::set<Entry> timers_;

    //所有活动定时器集合(未到期的定时器)
    std::set<ActiveTimer> activeTimers_;

    //取消定时器集合
    std::set<ActiveTimer> cancelligTimer_;

    std::atomic<bool> handleTimer_ = false;

    //当前已臂装的对齐后到期时间（微秒），0 表示未臂装（仅 loop 线程访问）
    int64_t armedUs_ = 0;
};
#endif