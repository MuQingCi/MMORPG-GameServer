#include "metrics/metricsSampler.h"
#include "common/msg.h"

#include <algorithm>

MetricsSampler::MetricsSampler(uint64_t stallMs, uint32_t unhealthySamples, uint32_t overloadPermille)
    : stallMs_(stallMs), unhealthySamples_(std::max(1u, unhealthySamples)),
      overloadPermille_(std::min(1000u, overloadPermille)) {}

SceneMetricsSnapshot MetricsSampler::sample(SceneMetrics& metrics, uint64_t now,
                                             const std::vector<uint64_t>& depths, uint64_t netDepth)
{
    SceneMetricsSnapshot result;
    result.sampledAtMs = now;
    result.netQueueDepth = netDepth;
    result.sampleSequence = ++sampleSequence_;
    result.sampleElapsedMs = lastSampleMs_ && now >= lastSampleMs_ ? now - lastSampleMs_ : 0;
    result.sampleValid = result.sampleElapsedMs != 0;
    const size_t count = metrics.workerCount();
    if (previousHandled_.size() != count)
    {
        previousHandled_.assign(count, 0); previousActiveUs_.assign(count, 0); stalledSamples_.assign(count, 0);
        previousQueueDepths_.assign(count, 0);
        overloadedSamples_.assign(count, 0);
        previousOverloadCandidate_.assign(count, 0);
        result.sampleValid = false;
    }
    result.workers.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        auto& metric = metrics.worker(i);
        const auto raw = metric.readRaw();
        const auto window = metric.collectWindow();
        const uint64_t depth = i < depths.size() ? depths[i] : 0;
        metric.setQueueDepth(depth);
        WorkerMetricsSnapshot item;
        item.handled = raw.handled; item.queueDepth = depth; item.lifecycle = raw.lifecycle; item.phase = raw.phase;
        item.lastProgressMs = raw.lastProgressMs;
        item.dropped = raw.dropped;
        item.luaReady = raw.luaReady;
        item.busyUs = raw.busyUs;
        item.maintenanceUs = raw.maintenanceUs;
        item.cycleUs = raw.cycleUs;
        item.waitUs = raw.waitUs;
        item.idleWaitUs = raw.idleWaitUs;
        item.enqueuedBusiness = raw.enqueuedBusiness;
        item.enqueuedInternal = raw.enqueuedInternal;
        item.dequeuedBusiness = raw.messageTypeCounts[MSGTYPE_CONN_DATA];
        for (uint32_t type = 1; type <= 100; ++type)
            if (type != MSGTYPE_CONN_DATA) item.dequeuedInternal += raw.messageTypeCounts[type];
        item.messageTypeCounts = raw.messageTypeCounts;
        item.dequeuedMessageTypeCounts = raw.messageTypeCounts;
        item.dequeuedBusiness = raw.messageTypeCounts[MSGTYPE_CONN_DATA];
        for (uint32_t type = 1; type <= 100; ++type)
            if (type != MSGTYPE_CONN_DATA) item.dequeuedInternal += raw.messageTypeCounts[type];
        item.serviceMethodCounts = raw.serviceMethodCounts;
        item.handlerMaxUsWindow = window.handlerMaxUs;
        item.cycleMaxUsWindow = window.cycleMaxUs;
        item.maxLoopUsWindow = window.maxLoopUs;
        item.players = raw.players;
        item.maxLoopUs = raw.maxLoopUs;
        result.playerCount += raw.players;
        result.totalHandled += raw.handled;
        result.totalHandlerUs += raw.handlerUsTotal;
        result.totalQueueDepth += depth;
        if (result.sampleValid)
        {
            const uint64_t handledDelta = raw.handled >= previousHandled_[i] ? raw.handled - previousHandled_[i] : 0;
            const uint64_t activeDelta = raw.activeUsTotal >= previousActiveUs_[i] ? raw.activeUsTotal - previousActiveUs_[i] : 0;
            item.handledPerSecond = handledDelta * 1000 / result.sampleElapsedMs;
            item.activePermille = std::min<uint64_t>(1000, activeDelta / result.sampleElapsedMs);
            result.windowHandled += handledDelta;
        }
        previousHandled_[i] = raw.handled; previousActiveUs_[i] = raw.activeUsTotal;
        bool stalled = false;
        if (raw.lifecycle == WorkerLifecycle::Running && raw.lastProgressMs && now >= raw.lastProgressMs)
            stalled = now - raw.lastProgressMs >= stallMs_;
        stalledSamples_[i] = stalled ? stalledSamples_[i] + 1 : 0;
        item.health = raw.lifecycle == WorkerLifecycle::Running
            ? (stalledSamples_[i] >= unhealthySamples_ ? WorkerHealth::SuspectedStall : WorkerHealth::Healthy)
            : WorkerHealth::Unknown;
        const bool growingQueue = depth > previousQueueDepths_[i];
        const bool overloadCandidate = result.sampleValid && item.activePermille >= overloadPermille_ && depth > 0;
        const bool overloaded = overloadCandidate && growingQueue && previousOverloadCandidate_[i];
        overloadedSamples_[i] = overloaded ? overloadedSamples_[i] + 1 : 0;
        item.overloaded = overloadedSamples_[i] >= unhealthySamples_;
        if (item.health == WorkerHealth::SuspectedStall) ++result.suspectedStallCount;
        if (item.overloaded) ++result.overloadedWorkerCount;
        result.workerActivePermilleAverage += item.activePermille;
        result.workerActivePermilleMax = std::max(result.workerActivePermilleMax, item.activePermille);
        previousOverloadCandidate_[i] = overloadCandidate ? 1 : 0;
        previousQueueDepths_[i] = depth;
        result.workers.push_back(item);
    }
    if (!result.workers.empty())
        result.workerActivePermilleAverage /= result.workers.size();
    if (result.sampleValid && result.sampleElapsedMs)
        result.handledPerSecond = result.windowHandled * 1000 / result.sampleElapsedMs;
    result.workerEquivalentCapacity = result.workerActivePermilleAverage;
    lastSampleMs_ = now;
    return result;
}