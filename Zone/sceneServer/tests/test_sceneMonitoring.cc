#include "metrics/sceneMonitoringService.h"
#include "common/msgBus.h"
#include "test_util.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace {
struct MonitoringFiles {
    std::filesystem::path directory;
    MonitoringFiles() {
        char pattern[] = "/tmp/scene-monitoring-XXXXXX";
        directory = ::mkdtemp(pattern);
    }
    ~MonitoringFiles() { std::filesystem::remove_all(directory); }
    std::string path() const { return (directory / "metrics.json").string(); }
};
std::string readFile(const std::string& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

TEST(MonitoringScheduleSkipsMissedDeadlinesWithoutInventingSamples)
{
    MonitoringSchedule schedule(true, 1000000);
    auto due = schedule.take(1000000);
    CHECK(due.metrics && due.diagnostic && due.fullDiagnostic);
    CHECK_EQ(schedule.nextUs(), 1010000u);
    due = schedule.take(1010000);
    CHECK(!due.metrics && due.diagnostic && !due.fullDiagnostic);
    due = schedule.take(2055000);
    CHECK(due.metrics && due.diagnostic && due.fullDiagnostic);
    CHECK_EQ(due.missedDiagnostic, 103u);
    CHECK_EQ(due.diagnosticLateUs, 1035000u);
    CHECK_EQ(schedule.nextUs(), 2060000u);
    due = schedule.take(2055000);
    CHECK(!due.metrics && !due.diagnostic);
    MonitoringSchedule normal(false, 1000000);
    normal.take(1000000);
    CHECK_EQ(normal.nextUs(), 2000000u);
    CHECK(!normal.take(1010000).diagnostic);
    CHECK_EQ(normal.take(4500000).missedMetrics, 2u);
}

TEST(MonitoringFastSamplesAndSnapshotReadsPreserveMetricsWindow)
{
    MonitoringFiles files;
    SceneMetrics metrics(1);
    MsgBus bus(1);
    bus.enableDiagnostics();
    SceneMonitoringService service(metrics, bus, files.path());
    CHECK(service.prepare());
    service.sampleDue(1000000);
    metrics.worker(0).recordHandlerDuration(777);
    metrics.worker(0).recordHandled(10);
    for (uint64_t now = 1010000; now < 2000000; now += 10000) {
        service.sampleDue(now);
        CHECK_EQ(service.snapshot().sampleSequence, 1u);
    }
    service.sampleDue(2000000);
    const auto snapshot = service.snapshot();
    CHECK_EQ(snapshot.workers[0].handlerMaxUsWindow, 777u);
    CHECK_EQ(snapshot.handledPerSecond, 10u);
    CHECK_EQ(snapshot.sampleElapsedMs, 1000u);
    CHECK_EQ(service.status().diagnosticSamples, 101u);
    CHECK(service.finish());
}

TEST(MonitoringRequiresDiagnosticPathButAllowsMemoryOnlyMetrics)
{
    SceneMetrics metrics(1);
    MsgBus bus(1);
    SceneMonitoringService memory(metrics, bus, "");
    CHECK(memory.prepare());
    memory.sampleDue(1000000);
    CHECK_EQ(memory.snapshot().workers.size(), 1u);
    CHECK(memory.finish());
    bus.enableDiagnostics();
    SceneMonitoringService invalid(metrics, bus, "");
    CHECK(!invalid.prepare());
    CHECK(!invalid.finish());
    SceneMonitoringService bad(metrics, bus, "/nonexistent-scene-monitoring/path");
    CHECK(!bad.prepare());
}

TEST(MonitoringMetricsWriteFailureDoesNotStopDiagnosticCollection)
{
    MonitoringFiles files;
    SceneMetrics metrics(1);
    MsgBus bus(1);
    bus.enableDiagnostics();
    SceneMonitoringService service(metrics, bus, files.path());
    CHECK(service.prepare());
    std::filesystem::create_directory(files.path()); // rename cannot replace a directory
    service.sampleDue(1000000);
    CHECK(service.status().metricsWriteFailed);
    CHECK(!service.status().diagnosticWriteFailed);
    CHECK_EQ(service.status().diagnosticSamples, 1u);
    CHECK(readFile(files.path() + ".queues.jsonl").find("\"fullSnapshot\":true") != std::string::npos);
    CHECK(service.finish()); // Preserve old semantics: metrics write failure is not a drain failure.
}

TEST(MonitoringDiagnosticFailureIsStickyAndMetricsContinue)
{
    MonitoringFiles files;
    SceneMetrics metrics(1);
    MsgBus bus(1);
    bus.enableDiagnostics();
    std::filesystem::create_symlink("/dev/full", files.path() + ".queues.jsonl");
    SceneMonitoringService service(metrics, bus, files.path());
    CHECK(service.prepare());
    service.sampleDue(1000000);
    CHECK(service.status().diagnosticWriteFailed);
    CHECK(bus.diagnostics()->writeFailed.load());
    metrics.worker(0).recordHandled(3);
    service.sampleDue(2000000);
    CHECK_EQ(service.snapshot().totalHandled, 3u);
    CHECK(!readFile(files.path()).empty());
    CHECK(!service.finish());
}

TEST(MonitoringOneThreadStopsIdempotentlyAndFinalSnapshotIncludesDrainEvents)
{
    MonitoringFiles files;
    SceneMetrics metrics(1);
    MsgBus bus(1);
    bus.enableDiagnostics();
    SceneMonitoringService service(metrics, bus, files.path());
    CHECK(service.prepare());
    CHECK(service.start());
    CHECK(service.start());
    service.stopPeriodic();
    service.stopPeriodic();
    const auto before = service.snapshot().sampleSequence;
    metrics.worker(0).recordHandled(5);
    bus.diagnostics()->event(0, "after_periodic_stop", 1, 2, 99);
    CHECK(service.finish());
    CHECK_EQ(service.snapshot().totalHandled, 5u);
    CHECK_EQ(service.snapshot().sampleSequence, before + 1);
    const auto output = readFile(files.path() + ".queues.jsonl");
    CHECK(output.find("after_periodic_stop") != std::string::npos);
    CHECK(service.finish());
    CHECK_EQ(readFile(files.path() + ".queues.jsonl"), output);
    CHECK(!service.start());
}

TEST(MonitoringReportsMissedDeadlinesAndRecoversMetricsExportWithoutClearingFailure)
{
    MonitoringFiles files;
    SceneMetrics metrics(1);
    MsgBus bus(1);
    bus.enableDiagnostics();
    SceneMonitoringService service(metrics, bus, files.path());
    CHECK(service.prepare());
    std::filesystem::create_directory(files.path());
    service.sampleDue(1000000);
    std::filesystem::remove(files.path());
    service.sampleDue(4055000);
    const auto state = service.status();
    CHECK(state.metricsWriteFailed);
    CHECK_EQ(state.metricsSamples, 2u);
    CHECK_EQ(state.diagnosticSamples, 2u);
    CHECK_EQ(state.missedMetrics, 2u);
    CHECK_EQ(state.missedDiagnostic, 304u);
    CHECK_EQ(state.maxDiagnosticGapUs, 3055000u);
    CHECK(readFile(files.path()).find("\"monitor_metricsWriteFailed\":true") != std::string::npos);
    CHECK(readFile(files.path()).find("\"monitor_missedDiagnostic\":304") != std::string::npos);
    CHECK(service.finish());
}

TEST(MonitoringMemoryOnlyLifecycleDoesNotEnableDiagnosticRecorder)
{
    SceneMetrics metrics(1);
    MsgBus bus(1);
    {
        SceneMonitoringService service(metrics, bus, "");
        CHECK(service.start());
        CHECK(bus.diagnostics() == nullptr);
        service.stopPeriodic();
        CHECK_EQ(service.status().diagnosticSamples, 0u);
        CHECK_EQ(service.snapshot().sampleSequence, 1u);
    } // Destructor joins but does not consume a final business window.
    metrics.worker(0).recordHandlerDuration(42);
    CHECK_EQ(metrics.worker(0).collectWindow().handlerMaxUs, 42u);
}