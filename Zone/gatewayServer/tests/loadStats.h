#ifndef CLEARMOON_GATEWAY_TESTS_LOAD_STATS_H
#define CLEARMOON_GATEWAY_TESTS_LOAD_STATS_H

#include <cstdint>
#include <mutex>
#include <vector>

struct LoadStatsSnapshot
{
    uint64_t attempted = 0;
    uint64_t completed = 0;
    uint64_t errors = 0;
    uint64_t timeouts = 0;
    uint64_t businessSucceeded = 0;
    uint64_t businessFailed = 0;
    uint64_t gatewayRejected = 0;
    uint64_t transportFailed = 0;
    uint64_t unknownResponses = 0;
    uint64_t decodeErrors = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t p50Us = 0;
    uint64_t p95Us = 0;
    uint64_t p99Us = 0;
    uint64_t minUs = 0;
    uint64_t maxUs = 0;
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
    void recordAttempt(uint64_t bytes);
    void recordResult(Result res, uint64_t latencyUs = 0, uint64_t bytesReceived = 0);
    void recordSentBytes(uint64_t bytes);
    LoadStatsSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    uint64_t attempted_ = 0;
    uint64_t completed_ = 0;
    uint64_t errors_ = 0;

    uint64_t businessSucceeded_ = 0;
    uint64_t businessFailed_    = 0;
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