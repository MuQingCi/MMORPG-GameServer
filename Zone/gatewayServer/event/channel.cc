#include "event/channel.h"

#include "event/eventLoop.h"
#include <cassert>
#include <sys/epoll.h>

Channel::Channel(EventLoop* loop, int fd) : loop_(loop),
                                            fd_(fd),
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
        // errorCallback_ 通常会关闭并摘除 channel；不能再继续调用同一连接的
        // read/write 回调，否则会在一次 epoll 事件中二次访问已关闭的连接。
        return;
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
        return;
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
