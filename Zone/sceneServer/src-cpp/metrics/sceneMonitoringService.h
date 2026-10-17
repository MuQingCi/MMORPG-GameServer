#ifndef CLEARMOON_METRICS_SCENEMONITORINGSERVICE_H
#define CLEARMOON_METRICS_SCENEMONITORINGSERVICE_H

#include "metrics/metricsSampler.h"

#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class MsgBus;

// Pure deadline arithmetic, independently testable without sleeping or running a server.
class MonitoringSchedule
{
public:
    struct Due {
        bool metrics = false, diagnostic = false, fullDiagnostic = false;
        uint64_t missedMetrics = 0, missedDiagnostic = 0;
        uint64_t metricsLateUs = 0, diagnosticLateUs = 0;
    };
    MonitoringSchedule(bool diagnostics, uint64_t startUs);
    Due take(uint64_t nowUs);
    uint64_t nextUs() const;
private:
    bool diagnostics_;
    uint64_t metricsUs_, diagnosticUs_, fullUs_;
};

struct SceneMonitoringStatus
{
    bool metricsWriteFailed = false, diagnosticWriteFailed = false;
    bool metricsSampleFailed = false, diagnosticSampleFailed = false;
    uint64_t metricsSamples = 0, diagnosticSamples = 0;
    uint64_t missedMetrics = 0, missedDiagnostic = 0;
    uint64_t maxMetricsLateUs = 0, maxDiagnosticLateUs = 0, maxDiagnosticGapUs = 0;
    uint64_t maxMetricsTaskUs = 0, maxDiagnosticTaskUs = 0;
};

// Borrows recorders: they must outlive this service. Lifecycle methods have one owner.
// Business threads only update recorders; external readers use snapshot()/status().
class SceneMonitoringService
{
public:
    SceneMonitoringService(SceneMetrics& metrics, MsgBus& bus, std::string path);
    ~SceneMonitoringService();
    SceneMonitoringService(const SceneMonitoringService&) = delete;
    SceneMonitoringService& operator=(const SceneMonitoringService&) = delete;

    bool prepare();
    bool start();
    void stopPeriodic();
    // Call after business producers/consumers stop. Idempotent; destructor does not finalize.
    bool finish();
    SceneMetricsSnapshot snapshot() const;
    SceneMonitoringStatus status() const;
    // Deterministic scheduler entry; lifecycle owner/tests only, never concurrently with start().
    void sampleDue(uint64_t nowUs);

private:
    void run();
    void collectMetrics(uint64_t nowUs, const std::vector<size_t>& depths, size_t net);
    void collectDiagnostic(uint64_t nowUs, const std::vector<size_t>& depths, size_t net, bool full);
    void fail(bool SceneMonitoringStatus::*field, const char* message, bool diagnostic);
    void writeMetrics(const SceneMetricsSnapshot& snapshot);

    SceneMetrics& metrics_;
    MsgBus& bus_;
    std::string path_;
    MetricsSampler sampler_;
    std::unique_ptr<MonitoringSchedule> schedule_;
    std::ofstream diagnosticOutput_;
    uint64_t startUs_ = 0, previousDiagnosticUs_ = 0;
    bool prepared_ = false, finished_ = false, finalSucceeded_ = true;
    bool diagnostics_ = false;
    mutable std::mutex snapshotMutex_;
    SceneMetricsSnapshot snapshot_;
    SceneMonitoringStatus status_;
    std::mutex waitMutex_;
    std::condition_variable waitCv_;
    bool running_ = false;
    std::thread thread_;
};

#endif