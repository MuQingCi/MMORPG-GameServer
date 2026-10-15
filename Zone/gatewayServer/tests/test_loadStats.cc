#include "loadStats.h"
#include "test_util.h"

TEST(LoadStatsComputesCountersAndPercentiles)
{
    LoadStats stats;
    stats.recordSuccess(1000);
    stats.recordSuccess(2000);
    stats.recordSuccess(3000);
    stats.recordError();

    const auto snapshot = stats.snapshot();
    CHECK_EQ(snapshot.completed, (uint64_t)3);
    CHECK_EQ(snapshot.errors, (uint64_t)1);
    CHECK_EQ(snapshot.p50Us, (uint64_t)2000);
    CHECK_EQ(snapshot.p95Us, (uint64_t)3000);
    CHECK_EQ(snapshot.p99Us, (uint64_t)3000);
}