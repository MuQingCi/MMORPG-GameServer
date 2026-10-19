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

TEST(LoadStatsExportsMergeableLatencySamplesAndP90)
{
    LoadStats stats;
    for (uint64_t i = 1; i <= 10; ++i)
        stats.recordResult(Result::BusinessResponse, i * 100);
    stats.recordResult(Result::BusinessError, 99999);
    CHECK_EQ(stats.snapshot().p90Us, 900u);
    const auto samples = stats.latencySamples();
    CHECK_EQ(samples.size(), size_t(10));
    CHECK_EQ(samples.front(), 100u);
    CHECK_EQ(samples.back(), 1000u);
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

TEST(LoadStatsTracksLogoutLifecycleSeparately)
{
    LoadStats stats;
    stats.recordLogout(true);
    stats.recordLogout(false);
    const auto result = stats.snapshot();
    CHECK_EQ(result.logoutSucceeded, 1u);
    CHECK_EQ(result.logoutFailed, 1u);
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

TEST(LoadStatsTracksActualSendsAndSchedulingDelay)
{
    LoadStats stats;
    stats.recordSend(100);
    stats.recordSend(300);
    const auto result = stats.snapshot();
    CHECK_EQ(result.sent, 2u);
    CHECK_EQ(result.scheduleDelayUs, 400u);
    CHECK_EQ(result.maxScheduleDelayUs, 300u);
}
