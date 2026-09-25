#ifndef CLEARMOON_GATEWAY_EVENT_EVENTLOOPTHREADPOOL_H
#define CLEARMOON_GATEWAY_EVENT_EVENTLOOPTHREADPOOL_H

#include "base/noncopy.h"
#include "event/eventThread.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>


class EventLoopThreadPool : public noncopyable
{
public:
using ThreadInitCallback = std::function<void(EventLoop*)>;

    // EventLoopThreadPool(const ThreadInitCallback& cb, std::string name = std::string());

    EventLoopThreadPool(const ThreadInitCallback& cb, std::string name = std::string(), size_t maxNum = 0);

    ~EventLoopThreadPool();

    void start();

    EventLoop* getNextLoop();

private:
    // std::vector<EventLoopThread*> threads_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;

    std::vector<EventLoop*> loops_;

    ThreadInitCallback cb_;

    size_t curNum_;
    //static const size_t maxNum_;
    size_t threadNum_;
    int nextIdx_;

    bool started_;
    std::string name_;
};


#endif