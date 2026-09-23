#ifndef CLEARMOON_GATEWAY_EVENT_EVENTLOOP_H
#define CLEARMOON_GATEWAY_EVENT_EVENTLOOP_H

#include "common/noncopy.h"
#include "common/currentThread.h"   // Current::tid()（isInThread 内联函数需要完整定义）
#include "timer/timerId.h"
#include "timer/timerQueue.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/types.h>
#include <vector>

class Channel;
class Poller;
class Timestamp;

class EventLoop : public noncopyable
{
public:
using Func = std::function<void()>;
    EventLoop();
    ~EventLoop();

    void loop();
    void quit();
    void weakup();

    void runInLoop(Func cb);

    /**
     * @brief 将回调排入待执行队列，不在当前线程立即执行，待本轮事件批处理完成后执行
     *        用于延迟连接对象析构，避免 Channel::handleEvent 中对已析构对象的悬垂访问
     */
    void queueInLoop(Func cb);

    void assertInLoopThread();

    bool isInThread() const { return tid_ == Current::tid(); }

    void updateChannel(Channel* channel);

    void removeChannel(Channel* channel);

    // ========== 定时器接口 ==========
    /**
     * @brief 在指定时刻执行回调
     * @param when 绝对时间
     */
    TimerId runAt(Timestamp when, Func cb);

    /**
     * @brief 延迟 delay 秒后执行回调
     * @param delay 秒（支持小数）
     */
    TimerId runAfter(double delay, Func cb);

    /**
     * @brief 每隔 interval 秒执行一次回调
     * @param interval 秒（支持小数）
     */
    TimerId runEvery(double interval, Func cb);

    /**
     * @brief 取消定时器
     */
    void cancel(TimerId timerId);
    
private:
using ChannelList = std::vector<Channel*>; 
using FuncList = std::vector<std::function<void()>>;

    void handleWeakup();
    void doPendingFuncs();

    pid_t tid_;    
    std::mutex mutex_;

    std::atomic<bool> looping_;
    std::atomic<bool> quit_;
    std::atomic<bool> eventHanding_;
    std::atomic<bool> callingPendingFunc_;

    int weakFd_;
    std::unique_ptr<Channel> weakChannel_;

    ChannelList activeChannels_;

    FuncList pendingFunc_;

    std::unique_ptr<Poller> poller_;

    std::unique_ptr<TimerQueue> timerQueue_;
};
#endif
