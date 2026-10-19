#ifndef CLEARMOON_GATEWAY_TESTS_LOAD_STATS_H
#define CLEARMOON_GATEWAY_TESTS_LOAD_STATS_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct LoadStatsSnapshot
{
    uint64_t sent = 0;
    uint64_t scheduleDelayUs = 0;
    uint64_t maxScheduleDelayUs = 0;
    uint64_t attempted = 0;
    uint64_t loadAttempted = 0;
    uint64_t completed = 0;
    uint64_t inFlight = 0;
    uint64_t drainUnfinished = 0;
    uint64_t cutoffUnfinished = 0;
    uint64_t unresolved = 0;
    uint64_t drainFinalUnfinished = 0;
    uint64_t errors = 0;
    uint64_t timeouts = 0;
    uint64_t businessSucceeded = 0;
    uint64_t businessFailed = 0;
    uint64_t logoutSucceeded = 0;
    uint64_t logoutFailed = 0;
    uint64_t gatewayRejected = 0;
    uint64_t transportFailed = 0;
    uint64_t unknownResponses = 0;
    uint64_t decodeErrors = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t p50Us = 0;
    uint64_t p90Us = 0;
    uint64_t p95Us = 0;
    uint64_t p99Us = 0;
    uint64_t minUs = 0;
    uint64_t maxUs = 0;
    std::map<std::string, uint64_t> businessErrors;
};

enum class Result
{
    BusinessResponse,
    ControlResponse,
    BusinessError,
    GatewayReject,
    TransportError,
    Timeout,
    UnknownResponse,
    DecodeError
};

class LoadStats
{
public:
    void recordSend(uint64_t scheduleDelayUs);
    void recordAttempt(uint64_t bytes);
    void recordInFlightDelta(int64_t delta);
    void recordDrainUnfinished(uint64_t count);
    void recordCutoffUnfinished(uint64_t count);
    void recordUnresolved(uint64_t count);
    void recordDrainFinalUnfinished(uint64_t count);
    void recordBusinessError(const std::string& reason);
    void recordResult(Result res, uint64_t latencyUs = 0, uint64_t bytesReceived = 0);
    void recordSentBytes(uint64_t bytes);
    void recordLogout(bool success);
    LoadStatsSnapshot snapshot() const;
    std::vector<uint64_t> latencySamples() const;

private:
    mutable std::mutex mutex_;
    uint64_t sent_ = 0;
    uint64_t scheduleDelayUs_ = 0;
    uint64_t maxScheduleDelayUs_ = 0;
    uint64_t attempted_ = 0;
    uint64_t loadAttempted_ = 0;
    uint64_t completed_ = 0;
    uint64_t inFlight_ = 0;
    uint64_t drainUnfinished_ = 0;
    uint64_t cutoffUnfinished_ = 0;
    uint64_t unresolved_ = 0;
    uint64_t drainFinalUnfinished_ = 0;
    std::map<std::string, uint64_t> businessErrors_;
    uint64_t errors_ = 0;

    uint64_t businessSucceeded_ = 0;
    uint64_t businessFailed_    = 0;
    uint64_t logoutSucceeded_ = 0;
    uint64_t logoutFailed_ = 0;
    uint64_t gatewayRejected_   = 0;
    uint64_t transportFailed_   = 0;
    uint64_t timeouts_          = 0;
    uint64_t unknownResponses_  = 0;
    uint64_t decodeErrors_      = 0;

    uint64_t bytesSent_ = 0;
    uint64_t bytesReceived_ = 0;
    std::vector<uint64_t> latenciesUs_;
};

#endif