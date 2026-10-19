#pragma once

#include "common/msg.h"
#include <array>
#include <mutex>
#include <sstream>
#include <vector>

// 按需启用的拒绝事件记录器；成功请求不逐次读时钟或分配内存。
// Net 负责记录拒绝，监控线程负责排空；工作线程只发布一次 TID。
class BusyDiagnostics {
public:
    enum class Reason : size_t { Direct=0, TaskLimit=1, ByteLimit=2 };
    struct Event {
        uint64_t monotonicUs, worker, reason, playerId, seq, session, clientSession;
        uint64_t gatewayId, sceneId, clientEpoch, module, method;
        uint64_t businessTasks, businessBytes, lifecycleTasks, taskLimit, byteLimit;
    };
    struct Snapshot {
        std::vector<Event> events;
        std::array<uint64_t,3> counts{};
        std::vector<uint64_t> workerTids;
        uint64_t dropped=0;
    };
    explicit BusyDiagnostics(size_t workers,size_t capacity=4096)
        : capacity_(capacity) { state_.workerTids.resize(workers); state_.events.reserve(capacity); }
    void record(size_t worker,Reason reason,uint64_t now,const Msg& m,
                size_t tasks,size_t bytes,size_t lifecycle,size_t taskLimit,size_t byteLimit) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.counts.at(static_cast<size_t>(reason));
        if(state_.events.size()>=capacity_) { ++state_.dropped; return; }
        const auto& c=m.head.clientBinding;
        state_.events.push_back({now,worker,static_cast<uint64_t>(reason),m.head.playerId,
            m.head.seq,m.head.session,c.clientSession,c.gatewayId,c.sceneId,c.clientEpoch,
            m.head.Module,m.head.Method,tasks,bytes,lifecycle,taskLimit,byteLimit});
    }
    void workerThread(size_t worker,uint64_t tid) {
        std::lock_guard<std::mutex> lock(mutex_); state_.workerTids.at(worker)=tid;
    }
    Snapshot collect() {
        Snapshot result;
        result.events.reserve(capacity_);
        std::lock_guard<std::mutex> lock(mutex_);
        result.counts=state_.counts; result.dropped=state_.dropped; result.workerTids=state_.workerTids;
        result.events.swap(state_.events);
        return result;
    }
    static std::string json(const Snapshot& s) {
        std::ostringstream out;
        out<<"{\"dropped\":"<<s.dropped<<",\"counts\":[";
        for(size_t i=0;i<3;++i) { if(i) out<<','; out<<s.counts[i]; }
        out<<"],\"workerTids\":[";
        for(size_t i=0;i<s.workerTids.size();++i) { if(i) out<<','; out<<s.workerTids[i]; }
        out<<"],\"events\":[";
        for(size_t i=0;i<s.events.size();++i) {
            if(i) out<<',';
            const auto& e=s.events[i];
            out<<"{\"monotonicUs\":"<<e.monotonicUs<<",\"worker\":"<<e.worker
               <<",\"reason\":"<<e.reason<<",\"playerId\":"<<e.playerId<<",\"seq\":"<<e.seq
               <<",\"session\":"<<e.session<<",\"clientSession\":"<<e.clientSession
               <<",\"gatewayId\":"<<e.gatewayId<<",\"sceneId\":"<<e.sceneId
               <<",\"clientEpoch\":"<<e.clientEpoch<<",\"module\":"<<e.module<<",\"method\":"<<e.method
               <<",\"businessTasks\":"<<e.businessTasks<<",\"businessBytes\":"<<e.businessBytes
               <<",\"lifecycleTasks\":"<<e.lifecycleTasks<<",\"taskLimit\":"<<e.taskLimit
               <<",\"byteLimit\":"<<e.byteLimit<<'}';
        }
        out<<"]}"; return out.str();
    }
private:
    size_t capacity_;
    std::mutex mutex_;
    Snapshot state_;
};