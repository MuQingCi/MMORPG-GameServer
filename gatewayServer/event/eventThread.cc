#include "event/eventThread.h"
#include "event/eventLoop.h"

#include <cassert>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <utility>

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb, 
                                 std::string name) : loop_(nullptr),
                                                     started_(false),
                                                     exiting_(false),
                                                     joined_(false),
                                                     //   exiting_(false),
                                                     //   running_(false),
                                                     callback_(std::move(cb)),
                                                     name_(name)
                                  
{
}


EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    if(started_)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(loop_)
        {
            loop_->quit();
        }
    }

    // 未 start() 时 thread_ 是默认构造的（!joinable）；对不可 join 的线程调 join()
    // 会抛 std::system_error，而在析构函数里抛异常等同于 terminate。
    if(!joined_ && thread_.joinable())
    {
        thread_.join();
        joined_ = true;
    }
}


EventLoop* EventLoopThread::start()
{
    assert(!started_);
    thread_ = std::thread(&EventLoopThread::threadFunc, this);
    started_ = true;

    EventLoop* loop;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond.wait(lock,[this]{ return loop_ != nullptr; });
        loop = loop_;
    }

    return loop;
}

void EventLoopThread::threadFunc()
{
    EventLoop loop;

    std::string threadName = name_.empty()? "ClearMoon" : name_;

    //设置线程名称
    int ret = pthread_setname_np(pthread_self(), threadName.c_str());

    if(ret != 0)
    {
        //Log<<error
    }

    //若设置了线程初始化回调则调用
    if(callback_) callback_(&loop);

    //成员loop_指向生成的Eventloop且通知主线程
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond.notify_one();
    }

    loop.loop();

    //循环结束时将成员Loop_置空
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}
