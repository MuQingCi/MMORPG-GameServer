#include "timer/timer.h"

uint64_t Timer::s_numCreated_ = 0;

/**
 * @brief 重启定时器
 *        对于重复定时器，将到期时间增加 interval
 *        一次性定时器什么也不做
 */
void Timer::restart(Timestamp now)
{
    if (repeat_)
    {
        // 到期时间 = 当前时间 + 间隔
        int64_t micro = static_cast<int64_t>(interval_ * 1000000);
        expiration_ = Timestamp(now.getMicroSecond() + micro);
    }
    else
    {
        expiration_ = Timestamp::invalid();
    }
}