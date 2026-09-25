#ifndef CLEARMOON_GATEWAY_POLLER_POLLER_H
#define CLEARMOON_GATEWAY_POLLER_POLLER_H

#include "base/noncopy.h"
#include <map>
#include <vector>

class EventLoop;
class Channel;
class Timestamp;

using ChannelList = std::vector<Channel*>;

class Poller : public noncopyable
{    
public:
    Poller(EventLoop* loop) : loop_(loop)
    {}
    virtual ~Poller() = default;

    virtual Timestamp poll(int timeoutMs, ChannelList* channels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;

    void assertInThread() const;
    bool hasChannel(Channel* channel) const;

protected:
    EventLoop* loop_;
    std::map<int , Channel*> ChannelMap_;
};



#endif