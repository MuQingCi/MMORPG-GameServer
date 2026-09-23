#ifndef CLEARMOON_GATEWAY_TIMER_TIMER_H
#define CLEARMOON_GATEWAY_TIMER_TIMER_H

#include "common/noncopy.h"
#include "common/timestamp.h"

#include <cstdint>
#include <functional>

/**
 * @brief 单个定时器
 *
 * 命名空间：本类**保持全局作用域**，与项目其它网络类一致
 * （Timestamp / Channel / Poller / EventLoop / TcpConnection / Socket / InetAddress 都是全局类）。
 * 历史问题：它曾被放进 clearmoon::net，而 TimerId / TimerQueue 的前置声明在全局作用域，
 * 于是 ::Timer 与 clearmoon::net::Timer 成了两个不同类型（timer.cc / timerQueue.cc 因此编不过）。
 * 决策见 docs/顺序实现清单.md 阶段 0 任务 0.3：统一为全局，不引入"只此一处带命名空间"的不一致。
 */
class Timer : public noncopyable
{
public:
    using TimerCallback = std::function<void()>;

    Timer(TimerCallback cb, Timestamp when, double interval)
        : callback_(std::move(cb)),
          expiration_(when),
          interval_(interval),
          repeat_(interval > 0.0),
          sequence_(s_numCreated_++)
    {}

    void run() const
    {
        if (callback_) callback_();
    }

    void cancel() { cancelled_ = true; }
    bool isCancelled() const { return cancelled_; }
 
    Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    double interval() const { return interval_; }
    uint64_t sequence() const { return sequence_; }

    /**
     * @brief 重启定时器（仅对重复定时器）
     *        将到期时间增加一个 interval
     */
    void restart(Timestamp now);

private:
    TimerCallback callback_;    //计时器回调
    Timestamp expiration_;      //到期时间
    const double interval_;    // 秒，<=0 表示一次性
    const bool repeat_;        // 是否重复

    uint64_t sequence_;        //Timer持有的唯一序列号（用于TimerQueue中cancel定时器以防止ABA问题)

    bool cancelled_ = false;     //延迟删除标记

    static uint64_t s_numCreated_;//序列号
};

#endif