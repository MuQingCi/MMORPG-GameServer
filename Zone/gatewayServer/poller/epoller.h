#ifndef CLEARMOON_GATEWAY_POLLER_EPOLLER_H
#define CLEARMOON_GATEWAY_POLLER_EPOLLER_H

#include "poller/poller.h"
#include <vector>
#include <sys/epoll.h>

class Epoller : public Poller
{
public:
    explicit Epoller(EventLoop* loop);
    ~Epoller();

    Timestamp poll(int timeoutMs, ChannelList* activeChannels);
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

private:
    void fillActiveChannels(int numEvent, ChannelList* activeChannels);

    int epfd_;
    std::vector<::epoll_event> events_;    

    static const int kMaxEvents;
};

#endif