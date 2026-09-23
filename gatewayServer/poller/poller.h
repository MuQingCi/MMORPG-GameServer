#ifndef CLEARMOON_GATEWAY_POLLER_POLLER_H
#define CLEARMOON_GATEWAY_POLLER_POLLER_H

#include "common/noncopy.h"
#include <map>
#include <vector>


class EventLoop;
class Channel;
class Timestamp;

// ChannelList 提升为**全局**别名：epoller.h/.cc 与 eventLoop.h 都直接使用裸 `ChannelList`，
// 若只定义在 Poller 类内（Poller::ChannelList），这些使用处就看不到它（编译报 "has not been declared"）。
// 同一类型的别名重复定义是合法的，因此 eventLoop.h 里那份可以保留、无需同步改动。
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