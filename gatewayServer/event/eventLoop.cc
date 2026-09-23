#include "timer/timerQueue.h"
#include "event/eventLoop.h"
#include "event/channel.h"
#include "common/currentThread.h"
#include "poller/DefaultPoller.h"
#include "common/timestamp.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

EventLoop::EventLoop(): tid_(Current::tid()),
                        mutex_(),
                        looping_(false),
                        quit_(false),
                        eventHanding_(false),
                        callingPendingFunc_(false),
                        weakFd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)), 
                        weakChannel_(std::make_unique<Channel>(this, weakFd_)),
                        poller_(createDefaultPoller(this)),
                        timerQueue_(std::make_unique<TimerQueue>(this))
{
    weakChannel_->setReadCallback([this]{ handleWeakup(); });
    weakChannel_->enableReading();
}

EventLoop::~EventLoop()
{
    quit();

    // 析构函数体内提前移除 weakChannel_，防止隐式析构按成员声明
    // 反序销毁 poller_ 先于 weakChannel_ 被释放，导致 SEGV
    if (weakChannel_)
    {
        weakChannel_->remove();
        weakChannel_.reset();
    }
}

void EventLoop::loop()
{
    assertInLoopThread();
    looping_ = true;
    quit_ = false;

    while(!quit_)
    {
        activeChannels_.clear();

        Timestamp time;
        // 使用 -1 永久阻塞，由 timerfd / eventfd 唤醒
        time = poller_->poll(-1, &activeChannels_);
        
        eventHanding_ = true;

        for(auto ch : activeChannels_)
        {
            ch->handleEvent();
        }

        eventHanding_ = false;

        doPendingFuncs();
    }

    //防止loop循环退出后，pending中还有待处理回调
    doPendingFuncs();

    looping_ = false;
}


void EventLoop::quit()
{
    if (quit_) return;
    quit_ = true;
    if(!isInThread())
    {
        weakup();
    }
}


void EventLoop::weakup()
{
    uint64_t one = 1;
    size_t ret = ::write(weakFd_, &one, sizeof one);

    if(ret != sizeof(one))
    {
        //Log<< "error<<
    }
}


void EventLoop::runInLoop(Func cb)
{
    if(isInThread())
    {  
        cb();
    }
    else
    {
        queueInLoop(std::move(cb));
    }
}


void EventLoop::assertInLoopThread()
{
    if(!isInThread())
    {
        abort();
    }
}

void EventLoop::updateChannel(Channel* channel)
{
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel)
{
    poller_->removeChannel(channel);
}

// ========== 定时器接口实现 ==========

TimerId EventLoop::runAt(Timestamp when, Func cb)
{
    return timerQueue_->addTimer(std::move(cb), when, 0.0);
}

TimerId EventLoop::runAfter(double delay, Func cb)
{
    Timestamp when(Timestamp::now().getMicroSecond() + static_cast<int64_t>(delay * 1000000));
    return timerQueue_->addTimer(std::move(cb), when, 0.0);
}

TimerId EventLoop::runEvery(double interval, Func cb)
{
    Timestamp when(Timestamp::now().getMicroSecond() + static_cast<int64_t>(interval * 1000000));
    return timerQueue_->addTimer(std::move(cb), when, interval);
}

void EventLoop::cancel(TimerId timerId)
{
    timerQueue_->cancel(timerId);
}



void EventLoop::handleWeakup()
{
    uint64_t one = 1;
    size_t ret = ::read(weakFd_, &one, sizeof one);
    if( ret != sizeof(one))
    {
        //Log<< "error<<
    }
}

void EventLoop::doPendingFuncs()
{
    assert(!callingPendingFunc_);
    FuncList func;

    callingPendingFunc_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        func.swap(pendingFunc_);    
    }
    for(auto& f : func)
    {
        f(); 
    }
    callingPendingFunc_ = false;
}


void EventLoop::queueInLoop(Func cb)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunc_.push_back(std::move(cb));
    }

    if(!isInThread() || callingPendingFunc_)
    {
        weakup();
    }
}
