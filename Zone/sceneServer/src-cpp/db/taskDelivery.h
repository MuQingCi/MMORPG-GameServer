#ifndef SCENE_DB_TASK_DELIVERY_H
#define SCENE_DB_TASK_DELIVERY_H

#include "common/msg.h"
#include "common/msgBus.h"
#include <atomic>
#include <functional>
#include <unordered_map>

// All methods run on the owning thread. AwaitResult is never automatically
// re-executed: a timeout does not establish that the database did not commit.
class PendingDbTasks {
public:
    static constexpr size_t kMaxTasks = 65536;
    enum class State { PendingDispatch, AwaitResult, Completed, DispatchFailed };
    struct Task {
        Msg request;
        State state = State::PendingDispatch;
        int64_t nextTryAt = 0;
    };
    using Sender = std::function<DispatchResult(const Msg&)>;
    
    bool submit(Msg request, int64_t now, const Sender& send) {
        if (request.head.msgType != MSGTYPE_DB_TASK_MYSQL &&
            request.head.msgType != MSGTYPE_DB_TASK_REDIS) return false;
        if (!request.head.taskId) request.head.taskId = allocateId();
        if (tasks_.size() >= kMaxTasks && !tasks_.contains(request.head.taskId)) {
            auto completed = tasks_.begin();
            while (completed != tasks_.end() && completed->second.state != State::Completed) ++completed;
            if (completed == tasks_.end()) return false;
            tasks_.erase(completed);
        }
        auto [it, inserted] = tasks_.emplace(request.head.taskId, Task{request});
        if (!inserted) return same(it->second.request, request);
        const auto result = dispatch(it->second, now, send);
        if (result == DispatchResult::kInvalidTarget || result == DispatchResult::kInvalidMessage) {
            tasks_.erase(it);
            return false;
        }
        return true; // Registered, even if currently backpressured.
    }
    void tick(int64_t now, const Sender& send) {
        for (auto& [id, task] : tasks_) {
            (void)id;
            if (task.state == State::PendingDispatch && now >= task.nextTryAt)
                dispatch(task, now, send);
        }
    }
    bool complete(const Msg& result) {
        auto it = tasks_.find(result.head.taskId);
        if (it == tasks_.end() || it->second.state != State::AwaitResult) return false;
        const auto& h = it->second.request.head;
        if (h.playerId != result.head.playerId || h.epoch != result.head.epoch ||
            h.ctx != result.head.ctx || h.srcWorkerId != result.head.srcWorkerId ||
            result.head.msgType != (h.msgType == MSGTYPE_DB_TASK_MYSQL
                ? MSGTYPE_DB_RESULT_MYSQL : MSGTYPE_DB_RESULT_REDIS)) return false;
        it->second.state = State::Completed;
        return true;
    }
    const Task* find(uint64_t id) const {
        auto it = tasks_.find(id);
        return it == tasks_.end() ? nullptr : &it->second;
    }
    size_t size() const { return tasks_.size(); }
    static bool same(const Msg& a, const Msg& b) {
        return a.body == b.body && a.head.msgType == b.head.msgType &&
            a.head.dbShard == b.head.dbShard && a.head.srcWorkerId == b.head.srcWorkerId &&
            a.head.ctx == b.head.ctx && a.head.playerId == b.head.playerId &&
            a.head.epoch == b.head.epoch && a.head.session == b.head.session &&
            a.head.Module == b.head.Module && a.head.Method == b.head.Method && a.head.seq == b.head.seq;
    }
private:
    static uint64_t allocateId() {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    static DispatchResult dispatch(Task& task, int64_t now, const Sender& send) {
        const auto result = send(task.request);
        if (IsAccepted(result)) task.state = State::AwaitResult;
        else if (result == DispatchResult::kInvalidTarget || result == DispatchResult::kInvalidMessage)
            task.state = State::DispatchFailed;
        task.nextTryAt = now + 100;
        return result;
    }
    // Completed entries are retained until capacity requires eviction.
    std::unordered_map<uint64_t, Task> tasks_;
};

// Each DB channel owns a separate ledger. Completion and result delivery are
// distinct; retrying delivery never calls the SQL executor again. Deduplication
// is bounded by retained entries, not a durable exactly-once guarantee.
class DbResultLedger {
public:
    static constexpr size_t kMaxEntries = 65536;
    enum class Admission { New, Duplicate, Conflict };
    struct Entry { Msg request; Msg result; bool ready = false; bool delivered = false; };
    bool hasCapacity() {
        if (entries_.size() + legacy_.size() < kMaxEntries) return true;
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.delivered) { entries_.erase(it); return true; }
        }
        return false; // Stop consuming new work until result delivery progresses.
    }
    Admission accept(const Msg& request) {
        if (!request.head.taskId) return Admission::New; // Legacy caller.
        auto [it, inserted] = entries_.emplace(request.head.taskId, Entry{request, {}});
        if (inserted) return Admission::New;
        if (!PendingDbTasks::same(it->second.request, request)) return Admission::Conflict;
        if (it->second.ready) it->second.delivered = false;
        return Admission::Duplicate;
    }
    void finish(const Msg& request, Msg result) {
        // Legacy results also need delivery retry, but not request deduplication.
        if (!request.head.taskId) { legacy_.push_back(std::move(result)); return; }
        auto& entry = entries_.at(request.head.taskId);
        entry.result = std::move(result);
        entry.ready = true;
        entry.delivered = false;
    }
    template<class Sender> void flush(Sender send) {
        for (auto& [id, entry] : entries_) {
            (void)id;
            if (entry.ready && !entry.delivered)
                entry.delivered = IsAccepted(send(entry.result));
        }
        for (auto it = legacy_.begin(); it != legacy_.end();) {
            if (IsAccepted(send(*it))) it = legacy_.erase(it);
            else ++it;
        }
    }
private:
    std::unordered_map<uint64_t, Entry> entries_;
    std::vector<Msg> legacy_;
};
#endif