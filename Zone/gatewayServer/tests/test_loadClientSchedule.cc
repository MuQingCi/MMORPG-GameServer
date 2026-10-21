#include "loadClientSchedule.h"
#include "loadStats.h"
#include "test_util.h"

TEST(LoadScheduleReplacesOldDeadlineAndDoesNotAccumulate)
{
    LoadClientSchedule schedule;
    auto now = LoadClientSchedule::Time{};
    for (int i = 0; i < 1000; ++i) schedule.update(1, now + std::chrono::milliseconds(i));
    CHECK_EQ(schedule.size(), size_t(1));
    uint32_t id = 0;
    CHECK(!schedule.pop(now, id));
    CHECK_EQ(schedule.waitMs(now), 20);
    CHECK(schedule.pop(now + std::chrono::seconds(1), id));
    CHECK_EQ(id, 1u);
    CHECK_EQ(schedule.size(), size_t(0));
}

TEST(LoadScheduleHonorsPhasesCancellationAndSubmillisecondDeadlines)
{
    LoadClientSchedule schedule;
    auto now = LoadClientSchedule::Time{};
    schedule.update(1, now + std::chrono::milliseconds(2));
    schedule.update(2, now + std::chrono::microseconds(50));
    CHECK_EQ(schedule.waitMs(now), 1);
    uint32_t id = 0;
    CHECK(!schedule.pop(now, id));
    CHECK(schedule.pop(now + std::chrono::microseconds(50), id));
    CHECK_EQ(id, 2u);
    schedule.erase(1);
    CHECK_EQ(schedule.waitMs(now), 20);
    schedule.update(3, now);
    CHECK_EQ(schedule.waitMs(now), 0);
}

TEST(LoadCountersSnapshotDoesNotCalculatePercentiles)
{
    LoadStats stats;
    stats.recordAttempt(32);
    stats.recordResult(Result::BusinessResponse, 123);
    auto counts = stats.countersSnapshot();
    CHECK_EQ(counts.completed, 1u);
    CHECK_EQ(counts.p99Us, 0u);
    CHECK_EQ(stats.snapshot().p99Us, 123u);
    stats.recordFailure(Result::TransportError);
    CHECK_EQ(stats.countersSnapshot().completed, 1u);
    CHECK_EQ(stats.countersSnapshot().errors, 1u);
}