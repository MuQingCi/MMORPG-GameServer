#include "event/eventThreadPool.h"
#include "event/eventThread.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
//const size_t EventLoopThreadPool::maxNum_ = 100;


// EventLoopThreadPool::EventLoopThreadPool(const ThreadInitCallback& cb,std::string name) 
//                                         : cb_(std::move(cb)), 
//                                           curNum_(0),
//                                           threadNum_(100),
//                                           nextIdx_(0),
//                                           started_(false),
//                                           name_(name)
// {
    
// }
//测试情况下设置线程池大小为4，否则设置为std::thread::hardware_concurrency()
EventLoopThreadPool::EventLoopThreadPool(const ThreadInitCallback& cb, 
                                         std::string name, size_t maxNum) 
                                        : cb_(std::move(cb)), 
                                          curNum_(0),
                                          threadNum_(maxNum > 0? maxNum :   std::thread::hardware_concurrency()),
                                          nextIdx_(0),
                                          started_(false),
                                          name_(name)
{
    if(threadNum_ == 0) threadNum_ = 1;
}



EventLoopThreadPool::~EventLoopThreadPool()
{
    
}


void EventLoopThreadPool::start()
{
    if(started_) return;

    std::string ThreadPoolName = name_.empty()? "ClearMoonPool" : name_;

    for(size_t i = 0; i < threadNum_; ++i)
    {
        std::string name = ThreadPoolName + std::to_string(i);
        // EventLoopThread* threadPtr = new EventLoopThread(cb_, name);
        // threads_.push_back(threadPtr);
        threads_.emplace_back(std::make_unique<EventLoopThread>(cb_,name));

        EventLoop* loop = nullptr;
        loop = threads_[i]->start();
        loops_.push_back(loop);
        curNum_++;
    }
    started_ = true;
}

EventLoop* EventLoopThreadPool::getNextLoop()
{
    // assert(!threads_.empty());
    // if(nextIdx_ >= threads_.size()) 
    //     nextIdx_ = 0;
    // EventLoop* ioLoop = threads_[nextIdx_++]->getLoop();
    // return ioLoop;

    assert(!loops_.empty());
    // nextIdx_ 是 int（轮询下标，恒 >= 0）：显式转成 int 比较，避免写成 size_t 比较
    if(nextIdx_ >= static_cast<int>(loops_.size()))
        nextIdx_ = 0;
    return loops_[nextIdx_++];
}


