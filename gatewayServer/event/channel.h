#ifndef CLEARMOON_GATEWAY_EVENT_CHANNEL_H
#define CLEARMOON_GATEWAY_EVENT_CHANNEL_H

#include "common/noncopy.h"
#include "common/callbacks.h"

#include <sys/epoll.h>

class EventLoop;
struct epoll_event;

class Channel :public noncopyable
{
public:
    Channel(EventLoop* loop, int fd);
    ~Channel();

    //interfaces of public
    //Get/Set info
    int getFd() const { return fd_; }
    int getRevents() const { return revents_; }
    int getEvents() const { return events_; }
    int getIndex() const { return index_; }

    void setRevents(int revent) { revents_ = revent; }
    void setIndex(int idx) { index_ = idx; }

    //set variable
    void setWriteCallback(const WriteCallback& cb) { writeCallback_ = cb; }
    void setReadCallback(const ReadCallback& cb) { readCallback_ = cb; }
    void setErrorCallback(const ErrorCallback& cb) { errorCallback_ = cb; }

    //set events_
    void enableReading(){ events_ |= EPOLLIN; update(); }
    void disableReading() { events_ &= ~EPOLLIN; update(); }

    void enableWriting(){ events_ |= EPOLLOUT; update(); }
    void disableWriting(){ events_ &= ~EPOLLOUT; update(); }

    void disableAll(){ events_ = 0; update(); }

    //get the state of events_
    bool isReading() const { return events_ & EPOLLIN; }
    bool isWriting() const { return events_ & EPOLLOUT; }
    bool isNonEvent() const { return events_ == 0; }

    //other function
    
    void handleEvent();

    void remove();
private:
    void update();

    EventLoop* loop_;

    int fd_;
    int revents_;   //当前事件
    int events_;    //关注事件

    WriteCallback writeCallback_;
    ReadCallback readCallback_;
    ErrorCallback errorCallback_;

    bool added_;
    int index_;     //kDeleted = -1, kNew = 0, kAdded = 1
};

#endif