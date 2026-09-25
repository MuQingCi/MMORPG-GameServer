#ifndef CLEARMOON_GATEWAY_POLLER_DEFAULTPOLLER_H
#define CLEARMOON_GATEWAY_POLLER_DEFAULTPOLLER_H

#include "poller/poller.h"
#include "poller/epoller.h"

static Poller* createDefaultPoller(EventLoop* loop)
{
    return new Epoller(loop);
}


#endif