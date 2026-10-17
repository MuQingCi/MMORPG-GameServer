#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <memory>
#include <atomic>
#include <string>
#include <vector>

// Test-only diagnostics. No network/persistence representation.
class QueueDiagnostics {
public:
    static uint64_t nowUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static size_t bucket(uint64_t us) {
        constexpr uint64_t bounds[] = {100,500,1000,2000,5000,10000,20000,50000,100000};
        size_t i=0; while(i<9 && us>bounds[i]) ++i; return i;
    }
    struct Wait {
        uint64_t count=0, sumUs=0, maxUs=0, missing=0;
        std::array<uint64_t,10> bins{};
    };
    struct Event { uint32_t worker; const char* kind; uint64_t begin,end,count; };
    explicit QueueDiagnostics(size_t channels) {
        for(size_t i=0;i<channels;++i) channels_.push_back(std::make_unique<Channel>());
    }
    void admission(size_t channel,size_t result) {
        auto& c=*channels_.at(channel); std::lock_guard<std::mutex> lock(c.mutex);
        ++c.admissions[0]; ++c.admissions.at(result+1);
    }
    std::array<uint64_t,5> admissions(size_t channel) {
        auto& c=*channels_.at(channel); std::lock_guard<std::mutex> lock(c.mutex); return c.admissions;
    }
    std::atomic<bool> writeFailed{false};
    void dequeued(uint32_t worker,size_t category,uint64_t begin,uint64_t end) {
        auto& c=*channels_.at(worker);
        std::lock_guard<std::mutex> lock(c.mutex);
        auto& w=c.waits.at(category);
        if(!begin || end<begin) { ++w.missing; return; }
        const auto us=end-begin; ++w.count; w.sumUs+=us;
        if(us>w.maxUs) w.maxUs=us;
        ++w.bins[bucket(us)];
    }
    void event(uint32_t worker,const char* kind,uint64_t begin,uint64_t end,uint64_t count) {
        auto& c=*channels_.at(worker);
        std::lock_guard<std::mutex> lock(c.mutex);
        if(c.events.size()<4096) c.events.push_back({worker,kind,begin,end,count});
        else ++c.dropped;
    }
    Wait wait(uint32_t worker,size_t category) {
        auto& c=*channels_.at(worker);
        std::lock_guard<std::mutex> lock(c.mutex); return c.waits.at(category);
    }
    struct Snapshot {
        uint64_t monotonicUs = 0;
        std::vector<size_t> depths;
        size_t netQueue = 0;
        bool fullSnapshot = true, writeFailed = false;
        std::vector<std::array<Wait, 4>> waits;
        std::vector<std::array<uint64_t, 5>> admissions;
        std::vector<Event> events;
        uint64_t droppedEvents = 0;
    };
    // Cumulative counters; only a full collection drains the bounded event buffers.
    Snapshot collect(uint64_t now, const std::vector<size_t>& depths, size_t net, bool full = true) {
        Snapshot snapshot;
        snapshot.monotonicUs = now;
        snapshot.depths = depths;
        snapshot.netQueue = net;
        snapshot.fullSnapshot = full;
        snapshot.writeFailed = writeFailed.load();
        if (full) {
            snapshot.waits.reserve(channels_.size());
            snapshot.admissions.reserve(channels_.size());
        }
        if (full) for (auto& ptr : channels_) {
            auto& c = *ptr;
            std::vector<Event> events;
            {
                std::lock_guard<std::mutex> lock(c.mutex);
                snapshot.waits.push_back(c.waits);
                snapshot.admissions.push_back(c.admissions);
                events.swap(c.events);
                snapshot.droppedEvents += c.dropped;
            }
            snapshot.events.insert(snapshot.events.end(), events.begin(), events.end());
        }
        return snapshot;
    }
    // Compatibility wrapper; serialization happens after all channel locks are released.
    std::string json(uint64_t now, const std::vector<size_t>& depths, size_t net, bool full = true);

private:
    struct Channel {
        std::mutex mutex;
        std::array<Wait,4> waits{};
        std::array<uint64_t,5> admissions{}; // attempt, accepted, backpressure, invalid target, invalid message
        std::vector<Event> events;
        uint64_t dropped=0;
    };
    std::vector<std::unique_ptr<Channel>> channels_;
};