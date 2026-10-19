#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

// 由工作线程写入，监控线程读取原子变量；并发采样的跨字段值
// 为近似值，静止状态下的快照才是精确值。使用固定存储，不记录事件列表。
class AoiDiagnostics
{
public:
    enum class Stage : size_t { Query, BuildDiff, Serialize, ClientAdmission, Commit,
                                ObserverPass, RemotePublish, Count };
    enum class Counter : size_t { Cells, Candidates, Visible, Observers, PushEntities,
                                  Bytes, Accepted, Rejected, Count };
    struct Duration {
        uint64_t count = 0, sumUs = 0, maxUs = 0;
        std::array<uint64_t, 10> bins{};
    };
    struct Snapshot {
        std::array<Duration, static_cast<size_t>(Stage::Count)> stages{};
        std::array<uint64_t, static_cast<size_t>(Counter::Count)> counters{};
        uint64_t invalidTimes = 0;
    };
    static uint64_t nowUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void add(Counter counter, uint64_t value = 1) {
        counters_.at(static_cast<size_t>(counter)).fetch_add(value, std::memory_order_relaxed);
    }
    void duration(Stage stage, uint64_t begin, uint64_t end) {
        if (end < begin) { invalidTimes_.fetch_add(1, std::memory_order_relaxed); return; }
        auto& target = stages_.at(static_cast<size_t>(stage));
        const auto us = end - begin;
        constexpr uint64_t bounds[] = {100,500,1000,2000,5000,10000,20000,50000,100000};
        size_t bin = 0;
        while (bin < 9 && us > bounds[bin]) ++bin;
        target.bins[bin].fetch_add(1, std::memory_order_relaxed);
        target.sumUs.fetch_add(us, std::memory_order_relaxed);
        auto old = target.maxUs.load(std::memory_order_relaxed);
        while (old < us && !target.maxUs.compare_exchange_weak(old, us, std::memory_order_relaxed)) {}
    }
    Snapshot snapshot() const {
        Snapshot result;
        for (size_t i = 0; i < stages_.size(); ++i) {
            auto& out = result.stages[i];
            for (size_t j = 0; j < out.bins.size(); ++j) {
                out.bins[j] = stages_[i].bins[j].load(std::memory_order_relaxed);
                out.count += out.bins[j];
            }
            out.sumUs = stages_[i].sumUs.load(std::memory_order_relaxed);
            out.maxUs = stages_[i].maxUs.load(std::memory_order_relaxed);
        }
        for (size_t i = 0; i < counters_.size(); ++i)
            result.counters[i] = counters_[i].load(std::memory_order_relaxed);
        result.invalidTimes = invalidTimes_.load(std::memory_order_relaxed);
        return result;
    }
    class Scope {
    public:
        Scope(AoiDiagnostics* metrics, Stage stage) : metrics_(metrics), stage_(stage),
            begin_(metrics ? nowUs() : 0) {}
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { if (metrics_) metrics_->duration(stage_, begin_, nowUs()); }
    private:
        AoiDiagnostics* metrics_;
        Stage stage_;
        uint64_t begin_;
    };
private:
    struct AtomicDuration {
        std::atomic<uint64_t> sumUs{0}, maxUs{0};
        std::array<std::atomic<uint64_t>, 10> bins{};
    };
    std::array<AtomicDuration, static_cast<size_t>(Stage::Count)> stages_{};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(Counter::Count)> counters_{};
    std::atomic<uint64_t> invalidTimes_{0};
};