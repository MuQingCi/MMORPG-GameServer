#ifndef CLEARMOON_GATEWAY_COMMON_CURRENTTHREAD_H
#define CLEARMOON_GATEWAY_COMMON_CURRENTTHREAD_H

#include <sched.h>
#include <unistd.h>

extern thread_local int t_cacheTid;
extern thread_local char t_tidString[32];
extern thread_local int t_tidStringLength;
extern thread_local const char* t_threadName;

void cacheTid();

inline int tid()
{
    if(t_cacheTid == 0)
    {
        cacheTid();
    }
    return t_cacheTid;
}

inline const char* tidString() { return t_tidString; }
inline int tidStringLength() { return t_tidStringLength; }
inline const char* threadName() { return t_threadName; }

/**
 * @brief 线程信息门面（兼容 muduo 风格的 Current::tid() 调用）
 *
 * 为什么需要它：本模块对外提供的是**自由函数**（tid()/tidString()/...），
 * 而调用方（eventLoop.h/.cc）写的是 `Current::tid()`。两种风格只能留一种：
 *   - 改调用点：需要动多处使用，且 `Current` 这一形态在别处也可能被继续引用；
 *   - 加门面：一处新增、调用方零改动，且不改变自由函数的既有契约。
 * 这里采用后者。将来若统一改名，应把自由函数与门面一起处理。
 */
struct Current
{
    static int tid() { return ::tid(); }
    static const char* tidString() { return ::tidString(); }
    static int tidStringLength() { return ::tidStringLength(); }
    static const char* threadName() { return ::threadName(); }
    static void setThreadName(const char* name) { t_threadName = name; }
};


#endif