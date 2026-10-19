#pragma once

#include "common/msg.h"
#include <array>
#include <atomic>
#include <cstdint>

// 仅用于可选诊断，不记录无界的逐消息事件，也不增加生产者锁。
class WorkDiagnostics
{
public:
    struct Duration {
        uint64_t count=0, sumUs=0, maxUs=0;
        std::array<uint64_t,10> bins{};
    };
    struct Class {
        uint64_t attempted=0, accepted=0, rejected=0, firstRejectedUs=0;
        Duration handled;
    };
    struct Snapshot {
        std::array<Class,6> classes{};
        std::array<Duration,10> phases{};
        std::array<uint64_t,6> dbTasks{};
        std::array<uint64_t,2> dbScanned{};
        uint64_t dbSampleUs=0;
        std::array<uint64_t,10> netPending{};
        std::array<std::array<uint64_t,12>,2> dbProbe{};
    };
    static size_t category(uint16_t type) {
        if(type==MSGTYPE_CONN_DATA) return 0;
        if(IsDbResult(type) || type==MSGTYPE_DB_STATUS) return 1;
        if(type==MSGTYPE_SERVICE) return 2;
        if(type==MSGTYPE_TIMER_FIRE) return 3;
        if(type==MSGTYPE_CONN_NEW || type==MSGTYPE_CONN_CLOSE || type==MSGTYPE_SHUTDOWN) return 4;
        return 5;
    }
    void admit(size_t category, bool accepted, uint64_t now) {
        auto& value=classes_.at(category);
        (accepted ? value.accepted : value.rejected).fetch_add(1,std::memory_order_relaxed);
        if(!accepted) {
            uint64_t previous=value.firstRejectedUs.load(std::memory_order_relaxed);
            while((previous==0 || now<previous) && !value.firstRejectedUs.compare_exchange_weak(previous,now,std::memory_order_relaxed)) {}
        }
    }
    void complete(size_t category, uint64_t us) { classes_.at(category).handled.add(us); }
    // phases: 0 load/drain/reload, 1 flush, 2 AOI/recovery, 3 persistence/retry.
    // 细分子阶段：4 为 AOI tick，5 为恢复，6 为持久化，7 为派发，
    // 8 为控制操作。阶段 9 是诊断采样开销（不包含在阶段 3 中）。
    void maintenance(size_t phase, uint64_t us) { phases_.at(phase).add(us); }
    void dbSnapshot(const std::array<uint64_t,6>& counts, uint64_t now) {
        for(size_t i=0;i<counts.size();++i) dbTasks_[i].store(counts[i],std::memory_order_relaxed);
        dbSampleUs_.store(now,std::memory_order_relaxed);
    }
    void dbScan(size_t phase,uint64_t count) { dbScanned_.at(phase).fetch_add(count,std::memory_order_relaxed); }
    void dbProbe(size_t phase,const std::array<uint64_t,12>& values) {
        for(size_t i=0;i<values.size();++i) dbProbe_.at(phase)[i].fetch_add(values[i],std::memory_order_relaxed);
    }
    // Net 线程采样：直接拒绝/条数限制/字节限制导致的 busy、过期、业务条数/字节数、
    // 生命周期任务数、失效、内部错误、接纳数；跨字段快照为近似值。
    void netPending(const std::array<uint64_t,10>& values) {
        for(size_t i=0;i<values.size();++i) netPending_[i].store(values[i],std::memory_order_relaxed);
    }
    Snapshot snapshot() const {
        Snapshot result;
        for(size_t i=0;i<classes_.size();++i) {
            const auto& source=classes_[i]; auto& target=result.classes[i];
            target.accepted=source.accepted.load(std::memory_order_relaxed);
            target.rejected=source.rejected.load(std::memory_order_relaxed);
            target.attempted=target.accepted+target.rejected;
            target.firstRejectedUs=source.firstRejectedUs.load(std::memory_order_relaxed);
            target.handled=source.handled.read();
        }
        for(size_t i=0;i<phases_.size();++i) result.phases[i]=phases_[i].read();
        for(size_t i=0;i<dbTasks_.size();++i) result.dbTasks[i]=dbTasks_[i].load(std::memory_order_relaxed);
        for(size_t i=0;i<dbScanned_.size();++i) result.dbScanned[i]=dbScanned_[i].load(std::memory_order_relaxed);
        result.dbSampleUs=dbSampleUs_.load(std::memory_order_relaxed);
        for(size_t p=0;p<dbProbe_.size();++p) for(size_t i=0;i<dbProbe_[p].size();++i)
            result.dbProbe[p][i]=dbProbe_[p][i].load(std::memory_order_relaxed);
        for(size_t i=0;i<netPending_.size();++i) result.netPending[i]=netPending_[i].load(std::memory_order_relaxed);
        return result;
    }
private:
    struct AtomicDuration {
        std::atomic<uint64_t> count{0},sumUs{0},maxUs{0};
        std::array<std::atomic<uint64_t>,10> bins{};
        void add(uint64_t us) {
            constexpr uint64_t bounds[]={100,500,1000,2000,5000,10000,20000,50000,100000};
            size_t b=0; while(b<9 && us>bounds[b]) ++b;
            bins[b].fetch_add(1,std::memory_order_relaxed);
            sumUs.fetch_add(us,std::memory_order_relaxed);
            uint64_t old=maxUs.load(std::memory_order_relaxed);
            while(old<us && !maxUs.compare_exchange_weak(old,us,std::memory_order_relaxed)) {}
            count.fetch_add(1,std::memory_order_relaxed);
        }
        Duration read() const {
            Duration value;
            // 以各桶之和定义总数，避免将并发采样误称为精确的跨字段原子快照。
            for(size_t i=0;i<bins.size();++i) { value.bins[i]=bins[i].load(std::memory_order_relaxed); value.count+=value.bins[i]; }
            value.sumUs=sumUs.load(std::memory_order_relaxed); value.maxUs=maxUs.load(std::memory_order_relaxed);
            return value;
        }
    };
    struct AtomicClass {
        std::atomic<uint64_t> accepted{0},rejected{0},firstRejectedUs{0};
        AtomicDuration handled;
    };
    std::array<AtomicClass,6> classes_{};
    std::array<AtomicDuration,10> phases_{};
    std::array<std::atomic<uint64_t>,6> dbTasks_{};
    std::array<std::atomic<uint64_t>,2> dbScanned_{};
    std::atomic<uint64_t> dbSampleUs_{0};
    std::array<std::atomic<uint64_t>,10> netPending_{};
    std::array<std::array<std::atomic<uint64_t>,12>,2> dbProbe_{};
};