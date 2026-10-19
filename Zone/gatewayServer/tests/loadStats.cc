#include "loadStats.h"

#include <algorithm>
#include <cmath>

void LoadStats::recordSend(uint64_t delay)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++sent_;
    scheduleDelayUs_ += delay;
    maxScheduleDelayUs_ = std::max(maxScheduleDelayUs_, delay);
}

void LoadStats::recordAttempt(uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++attempted_;
    ++loadAttempted_;
    bytesSent_ += bytes;
}

void LoadStats::recordInFlightDelta(int64_t delta)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (delta >= 0)
        inFlight_ += static_cast<uint64_t>(delta);
    else
        inFlight_ -= std::min(inFlight_, static_cast<uint64_t>(-delta));
}

void LoadStats::recordDrainUnfinished(uint64_t count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    drainUnfinished_ += count;
}

void LoadStats::recordCutoffUnfinished(uint64_t count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cutoffUnfinished_ += count;
}

void LoadStats::recordUnresolved(uint64_t count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    unresolved_ += count;
}

void LoadStats::recordDrainFinalUnfinished(uint64_t count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    drainFinalUnfinished_ += count;
}

void LoadStats::recordBusinessError(const std::string& reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++businessErrors_[reason];
}


void LoadStats::recordResult(Result res, uint64_t latencyUs, uint64_t bytesReceived)
{
    std::lock_guard<std::mutex> lock(mutex_);
    switch (res)
    {
        case Result::BusinessResponse:
            ++businessSucceeded_;
            ++completed_;
            bytesReceived_ += bytesReceived;
            latenciesUs_.push_back(latencyUs);
            break;
        case Result::ControlResponse:
            ++completed_;
            bytesReceived_ += bytesReceived;
            latenciesUs_.push_back(latencyUs);
            break;
        case Result::BusinessError:
            ++businessFailed_;
            ++errors_;
            ++completed_;
            break;
        case Result::DecodeError:
            ++decodeErrors_;
            ++errors_;
            ++completed_;
            break;
        case Result::GatewayReject:
            ++gatewayRejected_;
            ++errors_;
            ++completed_;
            break;
        case Result::TransportError:
            ++transportFailed_;
            ++errors_;
            ++completed_;
            break;
        case Result::Timeout:
            ++timeouts_;
            ++errors_;
            ++completed_;
            break;
        case Result::UnknownResponse:
            ++unknownResponses_;
            ++errors_;
            ++completed_;
            break;
    }
}

void LoadStats::recordSentBytes(uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    bytesSent_ += bytes;
}

void LoadStats::recordLogout(bool success)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (success) ++logoutSucceeded_;
    else ++logoutFailed_;
}

std::vector<uint64_t> LoadStats::latencySamples() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latenciesUs_;
}

LoadStatsSnapshot LoadStats::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    LoadStatsSnapshot out;
    out.sent = sent_;
    out.scheduleDelayUs = scheduleDelayUs_;
    out.maxScheduleDelayUs = maxScheduleDelayUs_;
    out.attempted = attempted_;
    out.loadAttempted = loadAttempted_;
    out.completed = completed_;
    out.inFlight = inFlight_;
    out.drainUnfinished = drainUnfinished_;
    out.cutoffUnfinished = cutoffUnfinished_;
    out.unresolved = unresolved_;
    out.drainFinalUnfinished = drainFinalUnfinished_;
    out.businessErrors = businessErrors_;
    out.errors = errors_;
    out.timeouts = timeouts_;
    out.businessSucceeded = businessSucceeded_;
    out.businessFailed = businessFailed_;
    out.logoutSucceeded = logoutSucceeded_;
    out.logoutFailed = logoutFailed_;
    out.gatewayRejected = gatewayRejected_;
    out.transportFailed = transportFailed_;
    out.unknownResponses = unknownResponses_;
    out.decodeErrors = decodeErrors_;
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
    out.p90Us = percentile(0.90);
    out.p95Us = percentile(0.95);
    out.p99Us = percentile(0.99);
    return out;
}