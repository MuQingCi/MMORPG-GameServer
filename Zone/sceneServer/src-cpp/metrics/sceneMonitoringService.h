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

// 纯截止时间计算，无需休眠或启动服务器即可独立测试。
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

// 借用记录器，其生命周期必须长于本服务；生命周期方法由单一所有者调用。
// 业务线程只更新记录器；外部读取方使用 snapshot()/status()。
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
    // 业务生产者/消费者停止后调用；该方法幂等，析构函数不执行最终采集。
    bool finish();
    SceneMetricsSnapshot snapshot() const;
    SceneMonitoringStatus status() const;
    // 确定性的调度入口；仅供生命周期所有者或测试使用，不得与 start() 并发调用。
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
    std::ofstream busyOutput_;
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