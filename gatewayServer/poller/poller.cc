#include "poller/poller.h"

#include "event/eventLoop.h"
#include "event/channel.h"

// =========== Public =========== //

void Poller::assertInThread() const
{
    loop_->assertInLoopThread();
}


bool Poller::hasChannel(Channel* channel) const
{
    assertInThread();
    auto it = ChannelMap_.find(channel->getFd());
    return it != ChannelMap_.end() && it->second == channel;
}

