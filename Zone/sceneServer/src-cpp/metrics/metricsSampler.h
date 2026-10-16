#ifndef CLEARMOON_METRICS_SAMPLER_H
#define CLEARMOON_METRICS_SAMPLER_H

#include "metrics/sceneMetrics.h"
#include <array>
#include <cstdint>
#include <vector>

struct WorkerMetricsSnapshot
{
    uint64_t handled = 0, handledPerSecond = 0, activePermille = 0, queueDepth = 0;
    uint64_t lastProgressMs = 0;
    uint64_t dropped = 0;
    uint64_t busyUs = 0, maintenanceUs = 0, cycleUs = 0, waitUs = 0, idleWaitUs = 0;
    uint64_t maxLoopUs = 0, enqueuedBusiness = 0, enqueuedInternal = 0;
    uint64_t dequeuedBusiness = 0, dequeuedInternal = 0;
    std::array<uint64_t, 101> messageTypeCounts{}, dequeuedMessageTypeCounts{};
    std::array<uint64_t, 5> serviceMethodCounts{};
    uint64_t handlerMaxUsWindow = 0, cycleMaxUsWindow = 0;
    uint64_t maxLoopUsWindow = 0;
    uint64_t players = 0;
    bool overloaded = false;
    WorkerHealth health = WorkerHealth::Unknown;
    WorkerLifecycle lifecycle = WorkerLifecycle::NotStarted;
    WorkerPhase phase = WorkerPhase::None;
    bool luaReady = false;
};

struct SceneMetricsSnapshot
{
    bool sampleValid = false;
    uint64_t sampledAtMs = 0, sampleElapsedMs = 0, sampleSequence = 0, uptimeMs = 0;
    uint64_t playerCount = 0, windowHandled = 0, handledPerSecond = 0, netQueueDepth = 0;
    uint64_t totalHandled = 0, totalHandlerUs = 0, totalQueueDepth = 0;
    uint64_t workerActivePermilleAverage = 0, workerActivePermilleMax = 0;
    uint64_t workerEquivalentCapacity = 0;
    uint64_t suspectedStallCount = 0, overloadedWorkerCount = 0;
    std::vector<WorkerMetricsSnapshot> workers;
};

class MetricsSampler
{
public:
    MetricsSampler(uint64_t stallMs = 5000, uint32_t unhealthySamples = 3, uint32_t overloadPermille = 900);
    SceneMetricsSnapshot sample(SceneMetrics& metrics, uint64_t monotonicMs,
                                const std::vector<uint64_t>& queueDepths, uint64_t netQueueDepth = 0);
private:
    uint64_t stallMs_;
    uint32_t unhealthySamples_, overloadPermille_;
    uint64_t lastSampleMs_ = 0;
    uint64_t sampleSequence_ = 0;
    std::vector<uint64_t> previousHandled_, previousActiveUs_;
    std::vector<uint32_t> stalledSamples_;
    std::vector<uint64_t> previousQueueDepths_;
    std::vector<uint32_t> overloadedSamples_;
    std::vector<uint8_t> previousOverloadCandidate_;
};

#endif