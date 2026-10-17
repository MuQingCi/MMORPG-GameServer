#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <memory>
#include <atomic>
#include <sstream>
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
    // Cumulative histograms; interval distributions are computed by subtraction.
    std::string json(uint64_t now,const std::vector<size_t>& depths,size_t net, bool full=true) {
        std::vector<std::array<Wait,4>> waits_;
        std::vector<std::array<uint64_t,5>> admissions;
        std::vector<Event> events_;
        uint64_t droppedEvents_=0;
        if(full) for(auto& ptr:channels_) {
            auto& c=*ptr;
            std::array<Wait,4> waits;
            std::array<uint64_t,5> counts;
            std::vector<Event> events;
            uint64_t dropped;
            {
                std::lock_guard<std::mutex> lock(c.mutex);
                waits=c.waits; counts=c.admissions; events.swap(c.events); dropped=c.dropped;
            }
            waits_.push_back(waits); admissions.push_back(counts);
            events_.insert(events_.end(),events.begin(),events.end());
            droppedEvents_+=dropped;
        }
        std::ostringstream out; out<<"{\"monotonicUs\":"<<now<<",\"netQueue\":"<<net<<",\"depths\":[";
        for(size_t i=0;i<depths.size();++i) { if(i) out<<','; out<<depths[i]; }
        out<<"],\"fullSnapshot\":"<<(full?"true":"false")<<",\"writeFailed\":"<<(writeFailed.load()?"true":"false")<<",\"admissions\":[";
        for(size_t i=0;i<admissions.size();++i) {
            if(i) out<<',';
            out<<'[';
            for(size_t j=0;j<5;++j) { if(j) out<<','; out<<admissions[i][j]; } out<<']';
        }
        out<<"],\"waits\":[";
        for(size_t i=0;i<waits_.size();++i) for(size_t c=0;c<4;++c) {
            if(i||c) out<<',';
            const auto& w=waits_[i][c];
            out<<"{\"worker\":"<<i<<",\"category\":"<<c<<",\"count\":"<<w.count
               <<",\"sumUs\":"<<w.sumUs<<",\"maxUs\":"<<w.maxUs<<",\"missing\":"<<w.missing<<",\"bins\":[";
            for(size_t b=0;b<10;++b) { if(b) out<<','; out<<w.bins[b]; } out<<"]}";
        }
        out<<"],\"droppedEvents\":"<<droppedEvents_<<",\"events\":[";
        for(size_t i=0;i<events_.size();++i) {
            if(i) out<<',';
            const auto& e=events_[i];
            out<<"{\"worker\":"<<e.worker<<",\"kind\":\""<<e.kind<<"\",\"beginUs\":"<<e.begin
               <<",\"endUs\":"<<e.end<<",\"count\":"<<e.count<<'}';
        }
        events_.clear(); out<<"]}\n"; return out.str();
    }
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