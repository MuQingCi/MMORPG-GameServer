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
    CHECK_EQ(snapshot.completed, (uint64_t)10);
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

TEST(LoadStatsSettlesEveryTerminalAttempt)
{
    LoadStats stats;
    stats.recordAttempt(10);
    stats.recordAttempt(10);
    stats.recordAttempt(10);
    stats.recordAttempt(10);
    stats.recordResult(Result::BusinessError);
    stats.recordResult(Result::GatewayReject);
    stats.recordResult(Result::Timeout);
    stats.recordResult(Result::TransportError);
    const auto snapshot = stats.snapshot();
    CHECK_EQ(snapshot.attempted, (uint64_t)4);
    CHECK_EQ(snapshot.completed, (uint64_t)4);
    CHECK_EQ(snapshot.errors, (uint64_t)4);
}

TEST(LoadStatsReportsDrainAndInFlightState)
{
    LoadStats stats;
    stats.recordAttempt(12);
    stats.recordAttempt(12);
    stats.recordInFlightDelta(2);
    stats.recordResult(Result::BusinessResponse, 100);
    stats.recordInFlightDelta(-1);
    stats.recordDrainUnfinished(1);
    stats.recordCutoffUnfinished(1);
    const auto snapshot = stats.snapshot();
    CHECK_EQ(snapshot.attempted, (uint64_t)2);
    CHECK_EQ(snapshot.completed, (uint64_t)1);
    CHECK_EQ(snapshot.inFlight, (uint64_t)1);
    CHECK_EQ(snapshot.drainUnfinished, (uint64_t)1);
    CHECK_EQ(snapshot.cutoffUnfinished, (uint64_t)1);
}

TEST(LoadStatsAccumulatesDrainUnfinishedAcrossConnections)
{
    LoadStats stats;
    stats.recordDrainUnfinished(2);
    stats.recordDrainUnfinished(3);
    CHECK_EQ(stats.snapshot().drainUnfinished, (uint64_t)5);
    stats.recordDrainFinalUnfinished(1);
    stats.recordDrainFinalUnfinished(2);
    CHECK_EQ(stats.snapshot().drainFinalUnfinished, (uint64_t)3);
}

TEST(LoadStatsTracksUnresolvedAttemptsSeparately)
{
    LoadStats stats;
    stats.recordAttempt(4);
    stats.recordUnresolved(1);
    const auto snapshot = stats.snapshot();
    CHECK_EQ(snapshot.attempted, (uint64_t)1);
    CHECK_EQ(snapshot.completed, (uint64_t)0);
    CHECK_EQ(snapshot.unresolved, (uint64_t)1);
}

TEST(LoadStatsGroupsBusinessErrorReasons)
{
    LoadStats stats;
    stats.recordBusinessError("1002:position mismatch");
    stats.recordBusinessError("1002:position mismatch");
    stats.recordBusinessError("1002:too far");
    const auto errors = stats.snapshot().businessErrors;
    CHECK_EQ(errors.at("1002:position mismatch"), (uint64_t)2);
    CHECK_EQ(errors.at("1002:too far"), (uint64_t)1);
}
