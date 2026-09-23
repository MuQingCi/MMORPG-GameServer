#include "event/channel.h"

#include "event/eventLoop.h"
#include <cassert>
#include <sys/epoll.h>

Channel::Channel(EventLoop* loop, int fd) : loop_(loop),
                                            fd_(fd),
                                            // 初始化列表按成员声明顺序书写（revents_ 在 events_ 之前），
                                            // 否则 -Wreorder 会提示"实际顺序与书写不一致"，容易误导读者
                                            revents_(0), 
                                            events_(0),
                                            added_(false),
                                            index_(0)
{
}

Channel::~Channel()
{
    remove();
}

void Channel::handleEvent()
{
    //Error
    if((revents_ & EPOLLERR ))
    {
        if(errorCallback_) errorCallback_();
    }

    //Read
    if(revents_ & (EPOLLIN | EPOLLRDHUP | EPOLLPRI) )
    {
        if(readCallback_) readCallback_();
    }

    //HUP
    if((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN))
    {
        if(errorCallback_) errorCallback_();
    }


    //Write
    if(revents_ & EPOLLOUT)
    {
        if(writeCallback_) writeCallback_();
    }
}

void Channel::remove()
{
    // 如果已经不再 epoll 中，直接返回（不调用 assertInLoopThread）
    if(!added_)
        return;
    added_ = false;

    // 如果 index_ 已经是 kDeleted，说明 Epoller::updateChannel
    // 在 events==0 时内部已经调过 removeChannel，不再重复移除
    if(index_ == -1)
        return;

    // 只有在需要实际操作 epoll 时才断言线程
    loop_->assertInLoopThread();
    loop_->removeChannel(this);
}

void Channel::update()
{
    loop_->assertInLoopThread();

    if(loop_)
    {
        loop_->updateChannel(this);
        added_ = true;
    }
}
