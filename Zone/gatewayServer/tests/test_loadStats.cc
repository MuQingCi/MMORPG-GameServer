#include "loadStats.h"
#include "test_util.h"

TEST(LoadStatsComputesCountersAndPercentiles)
{
    LoadStats stats;
    stats.recordResult(Result::BusinessResponse, 1000);
    stats.recordResult(Result::BusinessResponse, 2000);
    stats.recordResult(Result::BusinessResponse, 3000);
    stats.recordResult(Result::ControlResponse, 4000);
    stats.recordResult(Result::TransportError);
    stats.recordResult(Result::BusinessError);
    stats.recordResult(Result::GatewayReject);
    stats.recordResult(Result::UnknownResponse);
    stats.recordResult(Result::DecodeError);
    stats.recordResult(Result::Timeout);

    const auto snapshot = stats.snapshot();
    CHECK_EQ(snapshot.completed, (uint64_t)4);
    CHECK_EQ(snapshot.businessSucceeded, (uint64_t)3);
    CHECK_EQ(snapshot.errors, (uint64_t)6);
    CHECK_EQ(snapshot.businessSucceeded, (uint64_t)3);
    CHECK_EQ(snapshot.businessFailed, (uint64_t)1);
    CHECK_EQ(snapshot.gatewayRejected, (uint64_t)1);
    CHECK_EQ(snapshot.transportFailed, (uint64_t)1);
    CHECK_EQ(snapshot.unknownResponses, (uint64_t)1);
    CHECK_EQ(snapshot.decodeErrors, (uint64_t)1);
    CHECK_EQ(snapshot.timeouts, (uint64_t)1);
    CHECK_EQ(snapshot.p50Us, (uint64_t)2000);
    CHECK_EQ(snapshot.p95Us, (uint64_t)4000);
    CHECK_EQ(snapshot.p99Us, (uint64_t)4000);
}