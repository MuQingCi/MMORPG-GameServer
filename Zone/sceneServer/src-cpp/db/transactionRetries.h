#ifndef SCENE_DB_TRANSACTION_RETRIES_H
#define SCENE_DB_TRANSACTION_RETRIES_H

#include "common/msg.h"
#include "db/dbValue.h"
#include <deque>

// DB 所属线程独占的有限重试调度器，绝不重试提交结果未知的任务。
// 完成结果仍由 DbResultLedger 管理；重试条目不是完成结果。
class TransactionRetries 
{
public:
    struct Entry { Msg request; dbv::DbTask task; uint32_t attempts; int64_t nextAt; };
    static constexpr size_t kCapacity = 1024;
    static constexpr uint32_t kMaxAttempts = 3;

    bool schedule(const Msg& request, const dbv::DbTask& task, const dbv::DbResult& result,
                  uint32_t attempts, int64_t now) 
    {
        if ((task.kind != 4 && task.kind != 5) || result.classification != dbv::ResultClass::kRetryable ||
            attempts == 0 || attempts >= kMaxAttempts || entries_.size() >= kCapacity)
            return false;
        entries_.push_back({request,task,attempts,now + 100 * (int64_t(1) << (attempts-1))});
        return true;
    }
    template<class Execute, class Complete> 
    void tick(int64_t now, Execute execute, Complete complete) 
    {
        // 限制单轮工作量，避免到期重试独占通道。
        size_t budget = 16;
        for (auto it=entries_.begin(); it!=entries_.end() && budget;) 
        {
            if (now < it->nextAt) 
            { 
                ++it; 
                continue; 
            }

            auto entry=std::move(*it); 
            it=entries_.erase(it); 
            --budget;
            dbv::DbResult result; 
            execute(entry.task,result);
            ++entry.attempts;
            if (!schedule(entry.request,entry.task,result,entry.attempts,now))
                complete(entry.request,result);
            // deque 插入会使迭代器失效，因此插入后重新开始遍历。
            it=entries_.begin();
        }
    }
    size_t size() const { return entries_.size(); }
private:
    std::deque<Entry> entries_;
};
#endif