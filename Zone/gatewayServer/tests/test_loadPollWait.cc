#include "loadPollWait.h"
#include "test_util.h"

namespace
{
using Clock = std::chrono::steady_clock;
const auto now = Clock::time_point(std::chrono::seconds(100));
using namespace std::chrono_literals;
}

TEST(LoadPollWaitDoesNotWakeForBlockedSceneSend)
{
    CHECK_EQ(LoadPollWaitMs(now,now-5s,now+30s,now+40s,false,true),100);
    CHECK_EQ(LoadPollWaitMs(now,now+20ms,now+30s,now+40s,false,true),100);
}

TEST(LoadPollWaitPreservesSendingDeadlinesAndDrain)
{
    CHECK_EQ(LoadPollWaitMs(now,now+20ms,now+30s,now+40s,false,false),20);
    CHECK_EQ(LoadPollWaitMs(now,now-5s,now+30s,now+40s,false,false),1);
    CHECK_EQ(LoadPollWaitMs(now,now-5s,now+7ms,now+40s,false,true),7);
    CHECK_EQ(LoadPollWaitMs(now,now+20ms,now+7ms,now+40s,false,false),7);
    CHECK_EQ(LoadPollWaitMs(now,now-5s,now-1s,now+17ms,true,true),17);
    CHECK_EQ(LoadPollWaitMs(now,now-5s,now,now,true,true),1);
    CHECK_EQ(LoadPollWaitMs(now,now+20ms,now-1ms,now+40s,false,true),1);
    CHECK_EQ(LoadPollWaitMs(now,now+20ms,now-1ms,now+40s,false,false),1);
    CHECK_EQ(LoadPollWaitMs(now,now+999us,now+30s,now+40s,false,false),1);
}

TEST(LoadPollWaitBoundsStopSignalPolling)
{
    CHECK_EQ(LoadPollWaitMs(now,now+2s,now+30s,now+40s,false,false),100);
    CHECK_EQ(LoadPollWaitMs(now,now-5s,now,now+40s,true,true),100);
}