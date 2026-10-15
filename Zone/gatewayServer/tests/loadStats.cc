#include "loadStats.h"

#include <algorithm>
#include <cmath>

void LoadStats::recordAttempt(uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++attempted_;
    bytesSent_ += bytes;
}

void LoadStats::recordSuccess(uint64_t latencyUs, uint64_t bytesReceived)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++completed_;
    bytesReceived_ += bytesReceived;
    latenciesUs_.push_back(latencyUs);
}

void LoadStats::recordError()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++errors_;
}

void LoadStats::recordTimeout()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++timeouts_;
}

void LoadStats::recordSentBytes(uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    bytesSent_ += bytes;
}

LoadStatsSnapshot LoadStats::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    LoadStatsSnapshot out;
    out.attempted = attempted_;
    out.completed = completed_;
    out.errors = errors_;
    out.timeouts = timeouts_;
    out.bytesSent = bytesSent_;
    out.bytesReceived = bytesReceived_;
    if (latenciesUs_.empty())
        return out;

    std::vector<uint64_t> sorted = latenciesUs_;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double p) {
        const size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size())));
        const size_t index = std::min(sorted.size() - 1, rank == 0 ? size_t(0) : rank - 1);
        return sorted[index];
    };
    out.minUs = sorted.front();
    out.maxUs = sorted.back();
    out.p50Us = percentile(0.50);
    out.p95Us = percentile(0.95);
    out.p99Us = percentile(0.99);
    return out;
}