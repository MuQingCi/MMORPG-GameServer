#ifndef SCENE_DB_TASK_DELIVERY_H
#define SCENE_DB_TASK_DELIVERY_H

#include "common/msg.h"
#include "common/msgBus.h"
#include <atomic>
#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>

// 所有方法均在所属线程执行。AwaitResult 状态不会自动重新执行任务：超时不能证明数据库尚未提交。Sender 不得重入修改同一 PendingDbTasks 实例
class PendingDbTasks 
{
public:
    static constexpr size_t kMaxTasks = 65536;
    enum class State { PendingDispatch, AwaitResult, Completed, DispatchFailed };
    enum class SchedulingPolicy { LegacyScan, ActiveIndex };
    PendingDbTasks() = default;
    explicit PendingDbTasks(SchedulingPolicy policy) : indexed_(policy == SchedulingPolicy::ActiveIndex) {}
    struct Task {
        Msg request;
        State state = State::PendingDispatch;
        int64_t nextTryAt = 0;
        Msg result;
        bool ackPending = false;
        uint8_t observedStatus = 0; // 0=未知或记录未保留，1=处理中，2=已完成
        int64_t completedAt = 0;
    };
    using Sender = std::function<DispatchResult(const Msg&)>;
    struct ScanProbe {
        // 等待派发、等待结果、完成后等待 ACK、历史、失败。
        std::array<uint64_t,5> visited{};
        uint64_t callbacks=0,wallNs=0,cpuNs=0,callbackWallNs=0,callbackCpuNs=0,cpuClockFailures=0;
    };
    static uint64_t newTaskId() { return allocateId(); }
    SchedulingPolicy schedulingPolicy() const
    { return indexed_ ? SchedulingPolicy::ActiveIndex : SchedulingPolicy::LegacyScan; }
    // 兼容入口；明确拒绝运行开始后的配置。
    bool enableActiveIndex()
    {
        if(!tasks_.empty()) return false;
        indexed_=true;
        return true;
    }
    
    bool submit(Msg request, int64_t now, const Sender& send) ;

    bool observeStatus(const Msg& status);
    // 控制消息重试不会重新提交原 SQL 或 Redis 命令。
    void tickControls(int64_t now, const Sender& send, ScanProbe* probe = nullptr);

    static Msg control(const Msg& request, uint8_t kind) 
    {
        Msg message = request;
        message.head.msgType = request.head.msgType == MSGTYPE_DB_TASK_REDIS
            ? MSGTYPE_DB_CONTROL_REDIS : MSGTYPE_DB_CONTROL_MYSQL;
        message.head.dbControl = kind;
        return message;
    }

    bool acknowledgeDuplicate(const Msg& result);
    void tick(int64_t now, const Sender& send, ScanProbe* probe = nullptr);
    bool complete(const Msg& result) ;

    const Task* find(uint64_t id) const;

    size_t size() const { return tasks_.size(); }
    // 仅限所属线程；只用于诊断采样，绝不参与调度。
    // 总数、等待派发、等待结果、完成后等待 ACK、历史、失败。
    std::array<uint64_t,6> diagnosticSnapshot() const;

    bool unsettled() const;
    static bool same(const Msg& a, const Msg& b) 
    {
        return a.body == b.body && a.head.msgType == b.head.msgType &&
            a.head.dbShard == b.head.dbShard && a.head.srcWorkerId == b.head.srcWorkerId &&
            a.head.ctx == b.head.ctx && a.head.playerId == b.head.playerId &&
            a.head.epoch == b.head.epoch && a.head.session == b.head.session &&
            a.head.Module == b.head.Module && a.head.Method == b.head.Method && a.head.seq == b.head.seq;
    }
private:
    static bool needsScheduling(const Task& task)
    {
        return task.state == State::PendingDispatch || task.state == State::AwaitResult ||
            (task.state == State::Completed && task.ackPending);
    }
    void activate(uint64_t id) { if(indexed_) active_.insert(id); }
    void deactivate(uint64_t id) { if(indexed_) active_.erase(id); }
    static uint64_t allocateId() 
    {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    static DispatchResult dispatch(Task& task, int64_t now, const Sender& send) 
    {
        const auto result = send(task.request);
        if (IsAccepted(result)) 
            task.state = State::AwaitResult;
        else if (result == DispatchResult::kInvalidTarget || result == DispatchResult::kInvalidMessage)
            task.state = State::DispatchFailed;
        task.nextTryAt = now + (IsAccepted(result) ? 5000 : 100);
        return result;
    }
    // 已完成记录持续保留，直到容量压力需要回收。
    std::unordered_map<uint64_t, Task> tasks_;
    std::unordered_set<uint64_t> active_;
    bool indexed_=false;
};

// 每个 DB 通道独占一份记录表。执行完成与结果投递是
// 不同阶段；重试投递绝不再次调用 SQL 执行器。去重范围
// 受保留记录限制，不提供跨重启的持久恰好一次执行保证。
class DbResultLedger 
{
public:
    static constexpr size_t kMaxEntries = 65536;
    explicit DbResultLedger(size_t capacity = kMaxEntries);
    enum class Admission { New, Duplicate, Conflict };
    struct Entry { 
        Msg request; 
        Msg result; 
        bool ready = false; 
        bool delivered = false;
        bool acknowledged = false; 
        int64_t nextSendAt = 0; 
    };
    bool hasCapacity();
    Admission accept(const Msg& request);
    bool queueNotice(Msg notice);
    void finish(const Msg& request, Msg result);
    // 控制消息携带完整固定请求，防止错误归属的 ACK 确认任务。
    bool handleControl(const Msg& message);
    uint8_t status(uint64_t taskId) const;
    
    using Sender = std::function<DispatchResult(const Msg&)>;
    void flush(const Sender& send, int64_t now = NowMs());
private:
    size_t capacity_;
    std::unordered_map<uint64_t, Entry> entries_;
    std::vector<Msg> legacy_;
};
#endif