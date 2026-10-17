#include "metrics/sceneMetrics.h"
#include "metrics/metricsSampler.h"
#include "metrics/metricsJson.h"
#include "common/msg.h"
#include "test_util.h"
#include <thread>
#include <vector>

TEST(SceneMetricsWorkerCountersAndWindowMax)
{
    SceneMetrics metrics(2);
    auto& worker = metrics.worker(0);
    worker.recordHandled(3);
    worker.recordHandlerDuration(40);
    worker.recordHandlerDuration(12);
    worker.setPhase(WorkerPhase::Waiting, 100);

    auto raw = worker.readRaw();
    CHECK_EQ(raw.handled, 3u);
    CHECK_EQ(raw.handlerMaxUs, 40u);
    CHECK_EQ(raw.phase, WorkerPhase::Waiting);
    CHECK_EQ(raw.phaseStartedMs, 100u);
    CHECK_EQ(raw.handlerUsTotal, 52u);

    auto window = worker.collectWindow();
    CHECK_EQ(window.handlerMaxUs, 40u);
    CHECK_EQ(window.cycleMaxUs, 0u);
    CHECK_EQ(worker.collectWindow().handlerMaxUs, 0u);
    CHECK_EQ(worker.readRaw().handlerMaxUs, 40u);
    CHECK_EQ(worker.readRaw().handled, 3u);
}

TEST(MetricsSamplerPublishesWorkerCountersFromOneSource)
{
    SceneMetrics metrics(1);
    auto& worker = metrics.worker(0);
    worker.recordHandled(2);
    worker.recordDropped();
    worker.addBusyUs(30);
    worker.addMaintenanceUs(40);
    worker.addCycleUs(70);
    worker.addWaitUs(20, false);
    worker.addWaitUs(5, true);
    worker.recordCycleDuration(70);
    worker.recordMessageType(MSGTYPE_CONN_DATA, 3);
    worker.recordMessageType(MSGTYPE_SERVICE, 3);
    worker.setEnqueued(4, 6);
    worker.collectMaxLoopWindow(55);

    MetricsSampler sampler;
    const auto snapshot = sampler.sample(metrics, 1000, {7}, 9);
    const auto& sampled = snapshot.workers[0];
    CHECK_EQ(snapshot.netQueueDepth, 9u);
    CHECK_EQ(sampled.dropped, 1u);
    CHECK_EQ(sampled.busyUs, 30u);
    CHECK_EQ(sampled.maintenanceUs, 40u);
    CHECK_EQ(sampled.cycleUs, 70u);
    CHECK_EQ(sampled.waitUs, 25u);
    CHECK_EQ(sampled.idleWaitUs, 5u);
    CHECK_EQ(sampled.enqueuedBusiness, 4u);
    CHECK_EQ(sampled.enqueuedInternal, 6u);
    CHECK_EQ(sampled.dequeuedBusiness, 1u);
    CHECK_EQ(sampled.dequeuedInternal, 1u);
    CHECK_EQ(sampled.messageTypeCounts[MSGTYPE_SERVICE], 1u);
    CHECK_EQ(sampled.dequeuedMessageTypeCounts[MSGTYPE_SERVICE], 1u);
    CHECK_EQ(sampled.serviceMethodCounts[1], 0u);
    CHECK_EQ(sampled.serviceMethodCounts[2], 1u);
    CHECK_EQ(sampled.maxLoopUs, 55u);
    CHECK_EQ(sampled.maxLoopUsWindow, 55u);
    CHECK_EQ(sampled.cycleMaxUsWindow, 70u);
    CHECK_EQ(worker.collectWindow().cycleMaxUs, 0u);
}

TEST(MetricsSamplerDerivesDequeuedTotalsFromTypeCounters)
{
    SceneMetrics metrics(1);
    auto& worker = metrics.worker(0);
    worker.recordMessageType(MSGTYPE_CONN_DATA, 1);
    worker.recordMessageType(MSGTYPE_CONN_DATA, 2);
    worker.recordMessageType(MSGTYPE_TIMER_FIRE, 0);
    MetricsSampler sampler;
    const auto snapshot = sampler.sample(metrics, 10, {0});
    CHECK_EQ(snapshot.workers[0].dequeuedBusiness, 2u);
    CHECK_EQ(snapshot.workers[0].dequeuedInternal, 1u);
    CHECK_EQ(snapshot.workers[0].dequeuedMessageTypeCounts[MSGTYPE_TIMER_FIRE], 1u);
}

TEST(SceneMetricsJsonSerializationUsesOnlyStableSnapshot)
{
    SceneMetricsSnapshot snapshot;
    snapshot.sampleValid = true;
    snapshot.sampleSequence = 4;
    snapshot.totalHandled = 2;
    snapshot.workers.resize(1);
    snapshot.workers[0].messageTypeCounts[7] = 3;
    snapshot.workers[0].dequeuedMessageTypeCounts[7] = 3;
    const auto first = serializeSceneMetrics(snapshot);
    const auto second = serializeSceneMetrics(snapshot);
    CHECK_EQ(first, second);
    CHECK(first.find("\"worker0_type7\":3") != std::string::npos);
    CHECK(first.find("\"worker0_dequeuedType7\":3") != std::string::npos);
    CHECK(first.find("\"schemaVersion\":2") != std::string::npos);
}

TEST(MetricsSamplerCalculatesWorkerRateAndHealth)
{
    SceneMetrics metrics(1);
    MetricsSampler sampler(5000, 3, 900);
    auto& worker = metrics.worker(0);
    worker.setLifecycle(WorkerLifecycle::Running);
    worker.setPhase(WorkerPhase::Waiting, 1000);
    worker.setProgress(1000);
    auto first = sampler.sample(metrics, 1000, {0});
    CHECK(!first.sampleValid);

    worker.recordHandled(10);
    worker.addActiveUs(500000);
    worker.addHandlerDurationTotal(200000);
    worker.setPhase(WorkerPhase::Waiting, 1999);
    auto second = sampler.sample(metrics, 2000, {0});
    CHECK(second.sampleValid);
    CHECK_EQ(second.workers[0].handledPerSecond, 10u);
    CHECK_EQ(second.workers[0].activePermille, 500u);
    CHECK_EQ(second.windowHandled, 10u);
    CHECK_EQ(second.totalHandled, 10u);
    CHECK_EQ(second.totalHandlerUs, 200000u);
    CHECK_EQ(second.totalQueueDepth, 0u);
    CHECK_EQ(second.workerActivePermilleAverage, 500u);
    CHECK_EQ(second.workerActivePermilleMax, 500u);
    CHECK_EQ(second.workerEquivalentCapacity, 500u);
    CHECK_EQ(second.sampleSequence, 2u);
    CHECK_EQ(second.workers[0].health, WorkerHealth::Healthy);
}

TEST(MetricsSamplerFlagsStallOnlyAfterConsecutiveSamples)
{
    SceneMetrics metrics(1);
    MetricsSampler sampler(100, 2, 900);
    auto& worker = metrics.worker(0);
    worker.setLifecycle(WorkerLifecycle::Running);
    worker.setProgress(1000);
    sampler.sample(metrics, 1000, {0});
    CHECK_EQ(sampler.sample(metrics, 1100, {0}).workers[0].health, WorkerHealth::Healthy);
    CHECK_EQ(sampler.sample(metrics, 1200, {0}).workers[0].health, WorkerHealth::SuspectedStall);

    worker.setLifecycle(WorkerLifecycle::Stopped);
    CHECK_EQ(sampler.sample(metrics, 1300, {0}).workers[0].health, WorkerHealth::Unknown);
}

TEST(MetricsSamplerSeparatesOverloadFromHealth)
{
    SceneMetrics metrics(1);
    MetricsSampler sampler(5000, 3, 900);
    auto& worker = metrics.worker(0);
    worker.setLifecycle(WorkerLifecycle::Running);
    worker.setProgress(1000);
    sampler.sample(metrics, 1000, {1});
    worker.addActiveUs(950000);
    auto snapshot = sampler.sample(metrics, 2000, {5});
    CHECK(!snapshot.workers[0].overloaded);
    CHECK_EQ(snapshot.workers[0].health, WorkerHealth::Healthy);
    worker.addActiveUs(950000);
    snapshot = sampler.sample(metrics, 3000, {10});
    CHECK(!snapshot.workers[0].overloaded);
    worker.addActiveUs(950000);
    snapshot = sampler.sample(metrics, 4000, {15});
    CHECK(!snapshot.workers[0].overloaded);
    worker.addActiveUs(950000);
    snapshot = sampler.sample(metrics, 5000, {20});
    CHECK(snapshot.workers[0].overloaded);
}

TEST(SceneMetricsConcurrentWorkerUpdatesAreAtomic)
{
    SceneMetrics metrics(1);
    auto& worker = metrics.worker(0);
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i)
        writers.emplace_back([&worker] {
            for (int n = 0; n < 25000; ++n) worker.recordHandled();
        });
    for (auto& writer : writers) writer.join();
    CHECK_EQ(worker.readRaw().handled, 100000u);
}

TEST(MetricsSamplerAggregatesSceneAcrossWorkers)
{
    SceneMetrics metrics(2);
    MetricsSampler sampler;
    auto& first = metrics.worker(0);
    auto& second = metrics.worker(1);
    first.setLifecycle(WorkerLifecycle::Running);
    second.setLifecycle(WorkerLifecycle::Running);
    sampler.sample(metrics, 1000, {1, 2});
    first.recordHandled(4);
    first.recordHandlerDuration(200);
    first.addActiveUs(300000);
    second.recordHandled(6);
    second.recordHandlerDuration(100);
    second.addActiveUs(500000);
    const auto snapshot = sampler.sample(metrics, 2000, {3, 4});
    CHECK_EQ(snapshot.windowHandled, 10u);
    CHECK_EQ(snapshot.handledPerSecond, 10u);
    CHECK_EQ(snapshot.totalHandled, 10u);
    CHECK_EQ(snapshot.totalHandlerUs, 300u);
    CHECK_EQ(snapshot.totalQueueDepth, 7u);
    CHECK_EQ(snapshot.workerActivePermilleAverage, 400u);
    CHECK_EQ(snapshot.workerActivePermilleMax, 500u);
    CHECK_EQ(snapshot.workerEquivalentCapacity, 400u);
    CHECK_EQ(snapshot.sampleSequence, 2u);
}