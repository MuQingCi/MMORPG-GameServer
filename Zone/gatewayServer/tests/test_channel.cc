#include "event/channel.h"

#include <sys/epoll.h>

#include "test_util.h"

TEST(ChannelErrorStopsDispatch)
{
    Channel channel(nullptr, -1);
    int errors = 0;
    int reads = 0;
    int writes = 0;

    channel.setErrorCallback([&] { ++errors; });
    channel.setReadCallback([&] { ++reads; });
    channel.setWriteCallback([&] { ++writes; });
    channel.setRevents(EPOLLERR | EPOLLIN | EPOLLOUT);
    channel.handleEvent();

    CHECK_EQ(errors, 1);
    CHECK_EQ(reads, 0);
    CHECK_EQ(writes, 0);
}

TEST(ChannelHupWithoutReadStopsDispatch)
{
    Channel channel(nullptr, -1);
    int errors = 0;
    int reads = 0;
    int writes = 0;

    channel.setErrorCallback([&] { ++errors; });
    channel.setReadCallback([&] { ++reads; });
    channel.setWriteCallback([&] { ++writes; });
    channel.setRevents(EPOLLHUP | EPOLLOUT);
    channel.handleEvent();

    CHECK_EQ(errors, 1);
    CHECK_EQ(reads, 0);
    CHECK_EQ(writes, 0);
}