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
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t p50Us = 0;
    uint64_t p95Us = 0;
    uint64_t p99Us = 0;
    uint64_t minUs = 0;
    uint64_t maxUs = 0;
};

class LoadStats
{
public:
    void recordAttempt(uint64_t bytes);
    void recordSuccess(uint64_t latencyUs, uint64_t bytesReceived = 0);
    void recordError();
    void recordTimeout();
    void recordSentBytes(uint64_t bytes);
    LoadStatsSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    uint64_t attempted_ = 0;
    uint64_t completed_ = 0;
    uint64_t errors_ = 0;
    uint64_t timeouts_ = 0;
    uint64_t bytesSent_ = 0;
    uint64_t bytesReceived_ = 0;
    std::vector<uint64_t> latenciesUs_;
};

#endif