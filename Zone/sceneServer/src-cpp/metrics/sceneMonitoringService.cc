#include "metrics/sceneMonitoringService.h"

#include "common/msgBus.h"
#include "log/logger.h"
#include "metrics/metricsJson.h"
#include "metrics/queueDiagnosticsJson.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace {
bool takeDeadline(uint64_t now, uint64_t period, uint64_t& next, uint64_t& missed, uint64_t& late)
{
    if (now < next) return false;
    late = now - next;
    missed = late / period;
    next += (missed + 1) * period;
    return true;
}
}

MonitoringSchedule::MonitoringSchedule(bool diagnostics, uint64_t startUs)
    : diagnostics_(diagnostics), metricsUs_(startUs), diagnosticUs_(startUs), fullUs_(startUs) {}

MonitoringSchedule::Due MonitoringSchedule::take(uint64_t now)
{
    Due due;
    due.metrics = takeDeadline(now, 1000000, metricsUs_, due.missedMetrics, due.metricsLateUs);
    if (diagnostics_) {
        due.diagnostic = takeDeadline(now, 10000, diagnosticUs_, due.missedDiagnostic, due.diagnosticLateUs);
        uint64_t ignoredMissed = 0, ignoredLate = 0;
        due.fullDiagnostic = takeDeadline(now, 1000000, fullUs_, ignoredMissed, ignoredLate);
    }
    return due;
}

uint64_t MonitoringSchedule::nextUs() const
{
    return diagnostics_ ? std::min({metricsUs_, diagnosticUs_, fullUs_}) : metricsUs_;
}

SceneMonitoringService::SceneMonitoringService(SceneMetrics& metrics, MsgBus& bus, std::string path)
    : metrics_(metrics), bus_(bus), path_(std::move(path)), diagnostics_(bus.diagnostics() != nullptr) {}

SceneMonitoringService::~SceneMonitoringService() { stopPeriodic(); }

void SceneMonitoringService::fail(bool SceneMonitoringStatus::*field, const char* message, bool diagnostic)
{
    bool first;
    {
        std::lock_guard lock(snapshotMutex_);
        first = !(status_.*field);
        status_.*field = true;
    }
    if (diagnostic && bus_.diagnostics()) bus_.diagnostics()->writeFailed.store(true);
    if (first) LOG_ERROR << message; // One log per failure class, not one per tick.
}

bool SceneMonitoringService::prepare()
{
    if (finished_) return false;
    if (prepared_) return true;
    if (diagnostics_) {
        if (path_.empty()) {
            fail(&SceneMonitoringStatus::diagnosticWriteFailed, "queue diagnostics requires metricsPath", true);
            return false;
        }
        diagnosticOutput_.open(path_ + ".queues.jsonl", std::ios::trunc);
        if (!diagnosticOutput_) {
            fail(&SceneMonitoringStatus::diagnosticWriteFailed, "queue diagnostics open failed", true);
            return false;
        }
    }
    prepared_ = true;
    return true;
}

bool SceneMonitoringService::start()
{
    if (finished_) return false;
    {
        std::lock_guard lock(waitMutex_);
        if (running_) return true;
    }
    if (!prepare()) return false;
    // The initial sample stays synchronous, before exposing the background thread.
    sampleDue(QueueDiagnostics::nowUs());
    try {
        std::lock_guard lock(waitMutex_);
        running_ = true;
        thread_ = std::thread([this] { run(); });
    } catch (const std::exception&) {
        std::lock_guard lock(waitMutex_);
        running_ = false;
        fail(&SceneMonitoringStatus::metricsSampleFailed, "monitoring thread start failed", false);
        return false;
    }
    return true;
}

void SceneMonitoringService::stopPeriodic()
{
    {
        std::lock_guard lock(waitMutex_);
        running_ = false;
    }
    waitCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SceneMonitoringService::run()
{
    std::unique_lock lock(waitMutex_);
    while (running_) {
        const auto next = std::chrono::steady_clock::time_point(std::chrono::microseconds(schedule_->nextUs()));
        if (waitCv_.wait_until(lock, next, [this] { return !running_; })) break;
        lock.unlock();
        sampleDue(QueueDiagnostics::nowUs());
        lock.lock();
    }
}

SceneMetricsSnapshot SceneMonitoringService::snapshot() const
{
    std::lock_guard lock(snapshotMutex_);
    return snapshot_;
}

SceneMonitoringStatus SceneMonitoringService::status() const
{
    std::lock_guard lock(snapshotMutex_);
    return status_;
}

void SceneMonitoringService::sampleDue(uint64_t now)
{
    if (!prepared_ || finished_) return;
    if (!schedule_) {
        startUs_ = now;
        schedule_ = std::make_unique<MonitoringSchedule>(diagnostics_, now);
    }
    const auto due = schedule_->take(now);
    if (!due.metrics && !due.diagnostic) return;
    {
        std::lock_guard lock(snapshotMutex_);
        status_.missedMetrics += due.missedMetrics;
        status_.missedDiagnostic += due.missedDiagnostic;
        status_.maxMetricsLateUs = std::max(status_.maxMetricsLateUs, due.metricsLateUs);
        status_.maxDiagnosticLateUs = std::max(status_.maxDiagnosticLateUs, due.diagnosticLateUs);
        if (due.diagnostic && previousDiagnosticUs_ && now >= previousDiagnosticUs_)
            status_.maxDiagnosticGapUs = std::max(status_.maxDiagnosticGapUs, now - previousDiagnosticUs_);
    }
    // Share this tick's approximate queue depths without invoking the window sampler at 10ms.
    const auto depths = bus_.workerDepths();
    const auto net = bus_.netDepth();
    if (due.diagnostic) {
        previousDiagnosticUs_ = now;
        collectDiagnostic(now, depths, net, due.fullDiagnostic);
    }
    if (due.metrics) collectMetrics(now, depths, net);
}

void SceneMonitoringService::collectMetrics(uint64_t now, const std::vector<size_t>& depths, size_t net)
{
    const auto begin = QueueDiagnostics::nowUs();
    try {
        for (size_t id = 0; id < metrics_.workerCount(); ++id)
            metrics_.worker(id).setEnqueued(bus_.enqueuedBusinessCount(id), bus_.enqueuedInternalCount(id));
        const std::vector<uint64_t> queueDepths(depths.begin(), depths.end());
        auto sampled = sampler_.sample(metrics_, now / 1000, queueDepths, net);
        sampled.uptimeMs = now >= startUs_ ? (now - startUs_) / 1000 : 0;
        {
            std::lock_guard lock(snapshotMutex_);
            snapshot_ = sampled;
            ++status_.metricsSamples;
        }
        writeMetrics(sampled);
    } catch (const std::exception&) {
        fail(&SceneMonitoringStatus::metricsSampleFailed, "metrics sample failed", false);
    }
    std::lock_guard lock(snapshotMutex_);
    status_.maxMetricsTaskUs = std::max(status_.maxMetricsTaskUs, QueueDiagnostics::nowUs() - begin);
}

void SceneMonitoringService::writeMetrics(const SceneMetricsSnapshot& sampled)
{
    if (path_.empty()) return;
    const auto tmp = path_ + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    std::ofstream out(tmp, std::ios::trunc);
    if (out) {
        auto json = serializeSceneMetrics(sampled);
        const auto state = status();
        // Additive fields: old consumers still see exactly the same metrics and units.
        json.resize(json.size() - 2);
        out << json
            << ",\"monitor_metricsWriteFailed\":" << (state.metricsWriteFailed ? "true" : "false")
            << ",\"monitor_diagnosticWriteFailed\":" << (state.diagnosticWriteFailed ? "true" : "false")
            << ",\"monitor_metricsSampleFailed\":" << (state.metricsSampleFailed ? "true" : "false")
            << ",\"monitor_diagnosticSampleFailed\":" << (state.diagnosticSampleFailed ? "true" : "false")
            << ",\"monitor_missedMetrics\":" << state.missedMetrics
            << ",\"monitor_missedDiagnostic\":" << state.missedDiagnostic
            << ",\"monitor_maxMetricsLateUs\":" << state.maxMetricsLateUs
            << ",\"monitor_maxDiagnosticLateUs\":" << state.maxDiagnosticLateUs
            << ",\"monitor_maxDiagnosticGapUs\":" << state.maxDiagnosticGapUs
            << ",\"monitor_maxMetricsTaskUs\":" << state.maxMetricsTaskUs
            << ",\"monitor_maxDiagnosticTaskUs\":" << state.maxDiagnosticTaskUs << "}\n";
        out.close();
    }
    if (!out || ::rename(tmp.c_str(), path_.c_str()) != 0) {
        ::remove(tmp.c_str());
        fail(&SceneMonitoringStatus::metricsWriteFailed, "metrics write/rename failed", false);
    }
}

void SceneMonitoringService::collectDiagnostic(uint64_t now, const std::vector<size_t>& depths, size_t net, bool full)
{
    if (!diagnostics_ || status().diagnosticWriteFailed) return;
    const auto begin = QueueDiagnostics::nowUs();
    try {
        const auto sampled = bus_.diagnostics()->collect(now, depths, net, full);
        diagnosticOutput_ << serializeQueueDiagnostics(sampled);
        if (full) diagnosticOutput_.flush();
        if (!diagnosticOutput_)
            fail(&SceneMonitoringStatus::diagnosticWriteFailed, "queue diagnostics write failed", true);
        else {
            std::lock_guard lock(snapshotMutex_);
            ++status_.diagnosticSamples;
        }
    } catch (const std::exception&) {
        fail(&SceneMonitoringStatus::diagnosticSampleFailed, "queue diagnostics sample failed", true);
    }
    std::lock_guard lock(snapshotMutex_);
    status_.maxDiagnosticTaskUs = std::max(status_.maxDiagnosticTaskUs, QueueDiagnostics::nowUs() - begin);
}

bool SceneMonitoringService::finish()
{
    if (finished_) return finalSucceeded_;
    stopPeriodic();
    if (prepared_) {
        const auto now = QueueDiagnostics::nowUs();
        if (!schedule_) startUs_ = now;
        const auto depths = bus_.workerDepths();
        const auto net = bus_.netDepth();
        collectDiagnostic(now, depths, net, true);
        collectMetrics(now, depths, net);
        if (diagnosticOutput_.is_open()) {
            diagnosticOutput_.close();
            if (!diagnosticOutput_)
                fail(&SceneMonitoringStatus::diagnosticWriteFailed, "queue diagnostics close failed", true);
        }
    }
    finished_ = true;
    const auto state = status();
    finalSucceeded_ = !state.diagnosticWriteFailed && !state.diagnosticSampleFailed
        && (!diagnostics_ || !bus_.diagnostics()->writeFailed.load());
    return finalSucceeded_;
}