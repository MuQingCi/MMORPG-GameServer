#include "metrics/sceneMetrics.h"
#include "common/msg.h"

#include <stdexcept>

void WorkerMetrics::UpdateMax(std::atomic<uint64_t>& target, uint64_t value) noexcept
{
    uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

void WorkerMetrics::recordHandled(uint64_t n) noexcept { handled_.fetch_add(n, std::memory_order_relaxed); }
void WorkerMetrics::recordDropped(uint64_t n) noexcept { dropped_.fetch_add(n, std::memory_order_relaxed); }
void WorkerMetrics::recordHandlerDuration(uint64_t us) noexcept
{
    handlerUsTotal_.fetch_add(us, std::memory_order_relaxed);
    UpdateMax(handlerMaxUs_, us);
    UpdateMax(handlerWindowMaxUs_, us);
}
void WorkerMetrics::addHandlerDurationTotal(uint64_t us) noexcept
{
    handlerUsTotal_.fetch_add(us, std::memory_order_relaxed);
}
void WorkerMetrics::recordHandlerMax(uint64_t us) noexcept
{
    UpdateMax(handlerMaxUs_, us);
    UpdateMax(handlerWindowMaxUs_, us);
}
void WorkerMetrics::recordCycleDuration(uint64_t us) noexcept
{
    UpdateMax(cycleMaxUs_, us);
    UpdateMax(cycleWindowMaxUs_, us);
}
void WorkerMetrics::addActiveUs(uint64_t us) noexcept { activeUsTotal_.fetch_add(us, std::memory_order_relaxed); }
void WorkerMetrics::setProgress(uint64_t ms) noexcept { lastProgressMs_.store(ms, std::memory_order_relaxed); }
void WorkerMetrics::setPhase(WorkerPhase p, uint64_t ms) noexcept
{
    phaseStartedMs_.store(ms, std::memory_order_relaxed);
    phase_.store(p, std::memory_order_relaxed);
}
void WorkerMetrics::setLifecycle(WorkerLifecycle state) noexcept { lifecycle_.store(state, std::memory_order_relaxed); }
void WorkerMetrics::setLuaReady(bool ready) noexcept { luaReady_.store(ready, std::memory_order_relaxed); }
void WorkerMetrics::setCurrentMessage(uint32_t type, uint32_t method) noexcept
{
    currentType_.store(type, std::memory_order_relaxed);
    currentMethod_.store(method, std::memory_order_relaxed);
}
void WorkerMetrics::setPlayers(uint64_t n) noexcept { players_.store(n, std::memory_order_relaxed); }
void WorkerMetrics::setQueueDepth(uint64_t n) noexcept { queueDepth_.store(n, std::memory_order_relaxed); }
void WorkerMetrics::addBusyUs(uint64_t n) noexcept { busyUs_.fetch_add(n, std::memory_order_relaxed); }
void WorkerMetrics::addMaintenanceUs(uint64_t n) noexcept { maintenanceUs_.fetch_add(n, std::memory_order_relaxed); }
void WorkerMetrics::addCycleUs(uint64_t n) noexcept { cycleUs_.fetch_add(n, std::memory_order_relaxed); }
void WorkerMetrics::addWaitUs(uint64_t n, bool idle) noexcept
{
    waitUs_.fetch_add(n, std::memory_order_relaxed);
    if (idle) idleWaitUs_.fetch_add(n, std::memory_order_relaxed);
}
void WorkerMetrics::recordMessageType(uint32_t type, uint32_t method) noexcept
{
    if (type < messageTypeCounts_.size()) {
        messageTypeCounts_[type].fetch_add(1, std::memory_order_relaxed);
    }
    if (type == MSGTYPE_SERVICE) {
        constexpr uint32_t methods[] = {1, 2, 3, 5, 6};
        for (size_t i = 0; i < 5; ++i)
            if (method == methods[i]) serviceMethodCounts_[i].fetch_add(1, std::memory_order_relaxed);
    }
}
void WorkerMetrics::setEnqueued(uint64_t business, uint64_t internal) noexcept
{
    enqueuedBusiness_.store(business, std::memory_order_relaxed);
    enqueuedInternal_.store(internal, std::memory_order_relaxed);
}
void WorkerMetrics::collectMaxLoopWindow(uint64_t value) noexcept
{
    UpdateMax(maxLoopUs_, value);
    UpdateMax(maxLoopUsWindow_, value);
}
WorkerRawSnapshot WorkerMetrics::readRaw() const noexcept
{
    WorkerRawSnapshot result;
    result.handled = handled_.load(std::memory_order_relaxed);
    result.dropped = dropped_.load(std::memory_order_relaxed);
    result.handlerUsTotal = handlerUsTotal_.load(std::memory_order_relaxed);
    result.activeUsTotal = activeUsTotal_.load(std::memory_order_relaxed);
    result.lastProgressMs = lastProgressMs_.load(std::memory_order_relaxed);
    result.phaseStartedMs = phaseStartedMs_.load(std::memory_order_relaxed);
    result.handlerMaxUs = handlerMaxUs_.load(std::memory_order_relaxed);
    result.cycleMaxUs = cycleMaxUs_.load(std::memory_order_relaxed);
    result.players = players_.load(std::memory_order_relaxed);
    result.queueDepth = queueDepth_.load(std::memory_order_relaxed);
    result.maxLoopUs = maxLoopUs_.load(std::memory_order_relaxed);
    result.currentType = currentType_.load(std::memory_order_relaxed);
    result.currentMethod = currentMethod_.load(std::memory_order_relaxed);
    result.lifecycle = lifecycle_.load(std::memory_order_relaxed);
    result.phase = phase_.load(std::memory_order_relaxed);
    result.luaReady = luaReady_.load(std::memory_order_relaxed);
    result.busyUs = busyUs_.load(std::memory_order_relaxed);
    result.maintenanceUs = maintenanceUs_.load(std::memory_order_relaxed);
    result.cycleUs = cycleUs_.load(std::memory_order_relaxed);
    result.waitUs = waitUs_.load(std::memory_order_relaxed);
    result.idleWaitUs = idleWaitUs_.load(std::memory_order_relaxed);
    result.enqueuedBusiness = enqueuedBusiness_.load(std::memory_order_relaxed);
    result.enqueuedInternal = enqueuedInternal_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 101; ++i) {
        result.messageTypeCounts[i] = messageTypeCounts_[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 5; ++i) result.serviceMethodCounts[i] = serviceMethodCounts_[i].load(std::memory_order_relaxed);
    return result;
}
WorkerWindowSnapshot WorkerMetrics::collectWindow() noexcept
{
    return {handlerWindowMaxUs_.exchange(0, std::memory_order_relaxed),
            cycleWindowMaxUs_.exchange(0, std::memory_order_relaxed),
            maxLoopUsWindow_.exchange(0, std::memory_order_relaxed)};
}
SceneMetrics::SceneMetrics(size_t count)
{
    workers_.reserve(count);
    for (size_t i = 0; i < count; ++i) workers_.push_back(std::make_unique<WorkerMetricsStorage>());
}
WorkerMetrics& SceneMetrics::worker(size_t id)
{
    if (id >= workers_.size()) throw std::out_of_range("SceneMetrics worker id out of range");
    return workers_[id]->metrics;
}
const WorkerMetrics& SceneMetrics::worker(size_t id) const
{
    if (id >= workers_.size()) throw std::out_of_range("SceneMetrics worker id out of range");
    return workers_[id]->metrics;
}