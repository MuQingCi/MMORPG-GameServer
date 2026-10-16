#ifndef CLEARMOON_SCENE_METRICS_H
#define CLEARMOON_SCENE_METRICS_H

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

enum class WorkerLifecycle : uint8_t { NotStarted, Starting, Running, Draining, Stopped, Failed };
enum class WorkerPhase : uint8_t { None, Initializing, Maintenance, Waiting, Handling, Reloading, Stopping };
enum class WorkerHealth : uint8_t { Unknown, Healthy, SuspectedStall };

struct WorkerRawSnapshot
{
    uint64_t handled = 0, dropped = 0, handlerUsTotal = 0, activeUsTotal = 0;
    uint64_t lastProgressMs = 0, phaseStartedMs = 0;
    uint64_t handlerMaxUs = 0, cycleMaxUs = 0;
    uint64_t players = 0, queueDepth = 0;
    uint64_t busyUs = 0, maintenanceUs = 0, cycleUs = 0, waitUs = 0, idleWaitUs = 0;
    uint64_t maxLoopUs = 0, enqueuedBusiness = 0, enqueuedInternal = 0;
    std::array<uint64_t, 101> messageTypeCounts{};
    std::array<uint64_t, 5> serviceMethodCounts{};
    uint32_t currentType = 0, currentMethod = 0;
    WorkerLifecycle lifecycle = WorkerLifecycle::NotStarted;
    WorkerPhase phase = WorkerPhase::None;
    bool luaReady = false;
};

struct WorkerWindowSnapshot { uint64_t handlerMaxUs = 0, cycleMaxUs = 0, maxLoopUs = 0; };

class WorkerMetrics
{
public:
    WorkerMetrics() = default;
    WorkerMetrics(const WorkerMetrics&) = delete;
    WorkerMetrics& operator=(const WorkerMetrics&) = delete;

    void recordHandled(uint64_t count = 1) noexcept;
    void recordDropped(uint64_t count = 1) noexcept;
    void recordHandlerDuration(uint64_t us) noexcept;
    void addHandlerDurationTotal(uint64_t us) noexcept;
    void recordHandlerMax(uint64_t us) noexcept;
    void recordCycleDuration(uint64_t us) noexcept;
    void addActiveUs(uint64_t us) noexcept;
    void setProgress(uint64_t monotonicMs) noexcept;
    void setPhase(WorkerPhase phase, uint64_t monotonicMs) noexcept;
    void setLifecycle(WorkerLifecycle lifecycle) noexcept;
    void setLuaReady(bool ready) noexcept;
    void setCurrentMessage(uint32_t type, uint32_t method) noexcept;
    void setPlayers(uint64_t count) noexcept;
    void setQueueDepth(uint64_t depth) noexcept;
    void addBusyUs(uint64_t us) noexcept;
    void addMaintenanceUs(uint64_t us) noexcept;
    void addCycleUs(uint64_t us) noexcept;
    void addWaitUs(uint64_t us, bool idle) noexcept;
    void setEnqueued(uint64_t business, uint64_t internal) noexcept;
    void collectMaxLoopWindow(uint64_t value) noexcept;
    void recordMessageType(uint32_t type, uint32_t method) noexcept;
    WorkerRawSnapshot readRaw() const noexcept;
    WorkerWindowSnapshot collectWindow() noexcept;

private:
    static void UpdateMax(std::atomic<uint64_t>& target, uint64_t value) noexcept;
    alignas(64) std::atomic<uint64_t> handled_{0}, dropped_{0}, handlerUsTotal_{0}, activeUsTotal_{0};
    std::atomic<uint64_t> lastProgressMs_{0}, phaseStartedMs_{0};
    std::atomic<uint64_t> handlerMaxUs_{0}, cycleMaxUs_{0};
    std::atomic<uint64_t> handlerWindowMaxUs_{0}, cycleWindowMaxUs_{0};
    std::atomic<uint64_t> players_{0}, queueDepth_{0};
    std::atomic<uint64_t> busyUs_{0}, maintenanceUs_{0}, cycleUs_{0}, waitUs_{0}, idleWaitUs_{0};
    std::atomic<uint64_t> maxLoopUs_{0}, maxLoopUsWindow_{0}, enqueuedBusiness_{0}, enqueuedInternal_{0};
    std::array<std::atomic<uint64_t>, 101> messageTypeCounts_{};
    std::array<std::atomic<uint64_t>, 5> serviceMethodCounts_{};
    std::atomic<uint32_t> currentType_{0}, currentMethod_{0};
    std::atomic<WorkerLifecycle> lifecycle_{WorkerLifecycle::NotStarted};
    std::atomic<WorkerPhase> phase_{WorkerPhase::None};
    std::atomic<bool> luaReady_{false};
};

struct alignas(64) WorkerMetricsStorage
{
    WorkerMetrics metrics;
};

class SceneMetrics
{
public:
    explicit SceneMetrics(size_t workerCount);
    size_t workerCount() const noexcept { return workers_.size(); }
    WorkerMetrics& worker(size_t id);
    const WorkerMetrics& worker(size_t id) const;

private:
    std::vector<std::unique_ptr<WorkerMetricsStorage>> workers_;
};

#endif