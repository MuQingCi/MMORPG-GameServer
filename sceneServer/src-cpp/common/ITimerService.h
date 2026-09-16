#ifndef CLEARMOON_COMMON_ITIMERSERVICE_H
#define CLEARMOON_COMMON_ITIMERSERVICE_H

#include <cstdint>

struct TimerOp;

class ITimerService
{
public:
    virtual ~ITimerService() = default;
    virtual bool addTimer(const TimerOp& op) = 0;
    virtual bool cancel(uint64_t timerId) = 0;
};

#endif