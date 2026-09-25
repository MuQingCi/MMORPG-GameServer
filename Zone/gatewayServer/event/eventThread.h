#ifndef CLEARMOON_GATEWAY_EVENT_EVENTLOOPTHREAD_H
#define CLEARMOON_GATEWAY_EVENT_EVENTLOOPTHREAD_H

#include "base/noncopy.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>


class EventLoop;

class EventLoopThread : public noncopyable
{
public:
using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(), std::string name = std::string());
    
    ~EventLoopThread();

    EventLoop* start();
    
    void join()
    {
        // 未 start() 的线程不可 join（见 ~EventLoopThread 的说明）
        if(joined_ || !thread_.joinable()) return;
        thread_.join();
        joined_ = true;
    }
    
    EventLoop* getLoop() const
    {
        return loop_;
    }

private:
    void threadFunc();

    EventLoop* loop_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond;

    bool started_;
    bool exiting_;
    bool joined_;
    
    ThreadInitCallback callback_;
    std::string name_;

};

#endif