#pragma once

#include "common/msg.h"
#include "common/msgBus.h"
#include "gameProto.pb.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

//网络线程持有且遵循FIFO规则，拒绝会被返回且在队列迭代时从不通过回调发送：网络错误回复可能会关闭连接并重新进入取消状态
class WorkerPendingTasks {
public:
    struct Limits {
        size_t maxBusinessTasks=256, maxBusinessBytes=1024*1024;
        uint64_t businessTimeoutMs=500;
        size_t perWorkerAttempts=32, globalAttempts=128;
        uint32_t retryIntervalMs=10;
        bool valid() const {
            return maxBusinessTasks && maxBusinessBytes && businessTimeoutMs &&
                businessTimeoutMs<=3600000 && perWorkerAttempts && globalAttempts &&
                retryIntervalMs && retryIntervalMs<=1000;
        }
    };
    enum class Admission { Accepted, Pending, Busy, InternalError };
    struct Stats {
        size_t businessTasks=0,businessBytes=0,lifecycleTasks=0;
        uint64_t accepted=0,busy=0,expired=0,stale=0,internalErrors=0;
        uint64_t directBusy=0,taskLimitBusy=0,byteLimitBusy=0;
    };
    struct Rejected { Msg message; gs::ErrorCode code; };
    using Send=std::function<DispatchResult(size_t,const Msg&)>;
    using Valid=std::function<bool(const Msg&)>;

    WorkerPendingTasks(size_t workers, Limits limits) : queues_(workers),limits_(limits) 
    {
        if (!workers || !limits.valid()) 
            throw std::invalid_argument("invalid worker pending limits");
    }
    bool empty() const 
    {
        for (const auto& q:queues_) if (!q.tasks.empty()) 
            return false;
        return true;
    }

    const Stats& stats(size_t worker) const { return queues_.at(worker).stats; }

    Admission submit(size_t worker, const Msg& message, bool lifecycle, uint64_t now, const Send& send) 
    {
        if(worker>=queues_.size()) 
            return Admission::InternalError;
        auto& q=queues_[worker];
        if(q.tasks.empty()) 
        {
            const auto result=send(worker,message);
            if(IsAccepted(result)) 
            { 
                ++q.stats.accepted;
                return Admission::Accepted; 
            }
            if(result!=DispatchResult::kBackpressure) 
            {
                ++q.stats.internalErrors;
                if(!lifecycle)
                    return Admission::InternalError;
            }
            if(!lifecycle) 
            { 
                ++q.stats.busy;
                ++q.stats.directBusy;
                return Admission::Busy; 
            }
        }
        const size_t bytes=sizeof(Task)+message.body.size();
        if(!lifecycle && q.stats.businessTasks>=limits_.maxBusinessTasks) 
        {
            ++q.stats.busy;
            ++q.stats.taskLimitBusy;
            return Admission::Busy;
        }
        if(!lifecycle && (bytes>limits_.maxBusinessBytes || q.stats.businessBytes>limits_.maxBusinessBytes-bytes)) 
        {
            ++q.stats.busy;
            ++q.stats.byteLimitBusy;
            return Admission::Busy;
        }
        const uint64_t timeout=limits_.businessTimeoutMs*1000;
        const uint64_t deadline=now>std::numeric_limits<uint64_t>::max()-timeout ?
            std::numeric_limits<uint64_t>::max() : now+timeout;
        q.tasks.push_back({message,lifecycle,deadline,bytes});
        if(lifecycle) 
            ++q.stats.lifecycleTasks;
        else 
        { 
            ++q.stats.businessTasks;
            q.stats.businessBytes+=bytes; 
        }
        return Admission::Pending;
    }
    std::vector<Rejected> pump(uint64_t now, const Send& send, const Valid& valid) {
        std::vector<Rejected> rejected;
        // 有界业务扫描也会使阻塞的生命周期任务后方的任务超时。
        for(auto& q:queues_) 
            for(auto it=q.tasks.begin();it!=q.tasks.end();) 
            {
                if(!it->lifecycle && (now>=it->deadline || !valid(it->message))) 
                {
                    const bool expired=now>=it->deadline;
                    rejected.push_back({it->message,expired ? gs::ERROR_CODE_PENDING_TIMEOUT : gs::ERROR_CODE_PLAYER_SESSION_INVALID});
                    if(expired) ++q.stats.expired;else ++q.stats.stale;
                    accountRemove(q,*it);it=q.tasks.erase(it);
                } else ++it;
            }
        size_t total=0;
        for(size_t n=0;n<queues_.size() && total<limits_.globalAttempts;++n) 
        {
            const size_t worker=cursor_;
            cursor_=(cursor_+1)%queues_.size();
            auto& q=queues_[worker];
            for(size_t tried=0;!q.tasks.empty() && tried<limits_.perWorkerAttempts && total<limits_.globalAttempts;++tried) 
            {
                ++total;
                auto& task=q.tasks.front();
                const auto result=send(worker,task.message);
                if(result==DispatchResult::kBackpressure) break;
                if(!IsAccepted(result)) 
                {
                    ++q.stats.internalErrors;
                    // 不变量检查失败时仍保留生命周期任务，绝不能静默丢失。
                    if(task.lifecycle) 
                        break;
                    rejected.push_back({task.message,gs::ERROR_CODE_INTERNAL});
                } else ++q.stats.accepted;
                accountRemove(q,task);q.tasks.pop_front();
            }
        }
        return rejected;
    }
    void cancelSession(uint64_t session) 
    {
        for(auto& q:queues_) 
            for(auto it=q.tasks.begin();it!=q.tasks.end();) 
            {
                if(!it->lifecycle && it->message.head.session==session) 
                {
                    accountRemove(q,*it);it=q.tasks.erase(it);
                } else ++it;
            }
    }
    int waitMs(uint64_t now) const 
    {
        if(empty())
            return -1;
        uint64_t wait=limits_.retryIntervalMs;
        for(const auto& q:queues_) 
            for(const auto& task:q.tasks) 
                if(!task.lifecycle) 
                {
                    const auto ms=now>=task.deadline ? 0 : (task.deadline-now+999)/1000;
                    wait=std::min(wait,ms);
                }
        return static_cast<int>(wait);
    }
private:
    struct Task { Msg message;bool lifecycle;uint64_t deadline;size_t bytes; };
    struct Queue { std::deque<Task> tasks;Stats stats; };
    
    static void accountRemove(Queue& q,const Task& t) {
        if(t.lifecycle) --q.stats.lifecycleTasks;
        else { --q.stats.businessTasks;q.stats.businessBytes-=t.bytes; }
    }
    std::vector<Queue> queues_;
    Limits limits_;
    size_t cursor_=0;
};