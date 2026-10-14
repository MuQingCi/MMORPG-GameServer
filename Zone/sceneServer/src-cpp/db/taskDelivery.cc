#include "db/taskDelivery.h"


bool PendingDbTasks::submit(Msg request, int64_t now, const Sender& send) 
{
    if (request.head.msgType != MSGTYPE_DB_TASK_MYSQL &&
        request.head.msgType != MSGTYPE_DB_TASK_REDIS) 
            return false;
    if (!request.head.taskId) 
        request.head.taskId = allocateId();
    if (tasks_.size() >= kMaxTasks && !tasks_.contains(request.head.taskId)) 
    {
        auto completed = tasks_.begin();
        while (completed != tasks_.end() && 
        (completed->second.state != State::Completed ||
            completed->second.ackPending || 
            now - completed->second.completedAt < 60000)) 
            ++completed;
        if (completed == tasks_.end()) 
            return false;
        tasks_.erase(completed);
    }
    
    Task registered; 
    registered.request = request;
    
    auto [it, inserted] = tasks_.emplace(request.head.taskId, std::move(registered));
    if (!inserted) 
        return same(it->second.request, request);

    const auto result = dispatch(it->second, now, send);
    
    if (result == DispatchResult::kInvalidTarget || result == DispatchResult::kInvalidMessage) 
    {
        tasks_.erase(it);
        return false;
    }
    return true; // 即使当前遇到背压，也已完成任务登记。
}
bool PendingDbTasks::observeStatus(const Msg& status) 
{
    auto it = tasks_.find(status.head.taskId);
    if (it == tasks_.end() || it->second.state != State::AwaitResult ||
        status.head.msgType != MSGTYPE_DB_STATUS || status.body.size() != 1 ||
        static_cast<uint8_t>(status.body[0]) > 2) 
            return false;
    const auto& h = it->second.request.head;
    if (h.srcWorkerId != status.head.srcWorkerId || h.dbShard != status.head.dbShard ||
        h.playerId != status.head.playerId || h.epoch != status.head.epoch || h.ctx != status.head.ctx)
        return false;
    it->second.observedStatus = static_cast<uint8_t>(status.body[0]);
    // 未知状态不代表任务未执行；此处绝不重新提交原任务。
    return true;
}
// 控制消息重试不会重新提交原 SQL 或 Redis 命令。
void PendingDbTasks::tickControls(int64_t now, const Sender& send) 
{
    for (auto& [id, task] : tasks_) 
    {
        (void)id;
        if (now < task.nextTryAt) 
            continue;
        if (task.state == State::Completed && task.ackPending) 
        {
            if (IsAccepted(send(control(task.request, 1)))) 
                task.ackPending = false;
            task.nextTryAt = now + 100;
        } 
        else if (task.state == State::AwaitResult) {
            send(control(task.request, 2));
            task.nextTryAt = now + 5000;
        }
    }
}


bool PendingDbTasks::acknowledgeDuplicate(const Msg& result) 
{
    auto it = tasks_.find(result.head.taskId);
    if (it == tasks_.end() || it->second.state != State::Completed ||
        !same(it->second.result, result)) 
        return false;
    it->second.ackPending = true;
    it->second.nextTryAt = 0;
    return true;
}
void PendingDbTasks::tick(int64_t now, const Sender& send) 
{
    for (auto& [id, task] : tasks_) 
    {
        (void)id;
        if (task.state == State::PendingDispatch && now >= task.nextTryAt)
            dispatch(task, now, send);
    }
}
bool PendingDbTasks::complete(const Msg& result) 
{
    auto it = tasks_.find(result.head.taskId);
    if (it == tasks_.end() || it->second.state != State::AwaitResult) 
        return false;
    const auto& h = it->second.request.head;
    if (h.playerId != result.head.playerId || h.epoch != result.head.epoch ||
        h.dbShard != result.head.dbShard || h.Module != result.head.Module || h.Method != result.head.Method ||
        h.ctx != result.head.ctx || h.srcWorkerId != result.head.srcWorkerId ||
        result.head.msgType != (h.msgType == MSGTYPE_DB_TASK_MYSQL
            ? MSGTYPE_DB_RESULT_MYSQL : MSGTYPE_DB_RESULT_REDIS)) 
        return false;
    it->second.state = State::Completed;
    it->second.completedAt = NowMs();
    it->second.result = result;
    it->second.ackPending = true;
    it->second.nextTryAt = 0;
    return true;
}
const PendingDbTasks::Task* PendingDbTasks::find(uint64_t id) const 
{
    auto it = tasks_.find(id);
    return it == tasks_.end() ? nullptr : &it->second;
}

bool PendingDbTasks::unsettled() const 
{
    for(const auto& [id,task]:tasks_) 
    {
        (void)id;
        if(task.state!=State::Completed || task.ackPending) 
            return true;
    }
    return false;
}


DbResultLedger::DbResultLedger(size_t capacity) : capacity_(capacity) 
{}


bool DbResultLedger::hasCapacity() 
{
    if (entries_.size() + legacy_.size() < capacity_) 
        return true;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) 
    {
        if (it->second.acknowledged) 
        { 
            entries_.erase(it); 
            return true; 
        }
    }
    return false; // 暂停消费新任务，等待结果投递取得进展。
}
DbResultLedger::Admission DbResultLedger::accept(const Msg& request) 
{
    if (!request.head.taskId) 
        return Admission::New; // 兼容未携带任务身份的旧调用方。
    
    auto [it, inserted] = entries_.emplace(request.head.taskId, Entry{request, {}});

    if (inserted) 
        return Admission::New;

    if (!PendingDbTasks::same(it->second.request, request)) 
        return Admission::Conflict;

    if (it->second.ready) 
    { 
        it->second.delivered = false; 
        it->second.nextSendAt = 0;
        it->second.acknowledged = false; 
    }
    return Admission::Duplicate;
}
bool DbResultLedger::queueNotice(Msg notice) 
{
    if (entries_.size() + legacy_.size() >= capacity_) 
        return false;
    legacy_.push_back(std::move(notice));
    return true;
}
void DbResultLedger::finish(const Msg& request, Msg result) 
{
    // 旧协议结果同样需要投递重试，但不参与请求去重。
    if (!request.head.taskId) 
    { 
        legacy_.push_back(std::move(result)); 
        return; 
    }

    auto& entry = entries_.at(request.head.taskId);
    entry.result = std::move(result);
    entry.ready = true;
    entry.delivered = false;
}
// 控制消息携带完整固定请求，防止错误归属的 ACK 确认任务。
bool DbResultLedger::handleControl(const Msg& message) 
{
    if (message.head.msgType != MSGTYPE_DB_CONTROL_MYSQL &&
        message.head.msgType != MSGTYPE_DB_CONTROL_REDIS) return false;
    auto it = entries_.find(message.head.taskId);
    if (it == entries_.end()) 
        return false;
    auto request = message;
    request.head.msgType = message.head.msgType == MSGTYPE_DB_CONTROL_REDIS
        ? MSGTYPE_DB_TASK_REDIS : MSGTYPE_DB_TASK_MYSQL;
    request.head.dbControl = 0;
    if (!PendingDbTasks::same(it->second.request, request)) 
        return false;
    if (message.head.dbControl == 1 && it->second.ready) 
    {
        it->second.acknowledged = true;
        return true;
    }

    if (message.head.dbControl == 2) 
    {
        it->second.nextSendAt = 0;
        it->second.delivered = false;
        it->second.acknowledged = false;
        return true;
    }
    return false;
}
uint8_t DbResultLedger::status(uint64_t taskId) const 
{
    auto it = entries_.find(taskId);
    return it == entries_.end() ? 0 : (it->second.ready ? 2 : 1);
}

template<class Sender> 
void DbResultLedger::flush(Sender send, int64_t now) 
{
    for (auto& [id, entry] : entries_) 
    {
        (void)id;
        if (entry.ready && !entry.acknowledged && now >= entry.nextSendAt) 
        {
            entry.delivered = IsAccepted(send(entry.result));
            entry.nextSendAt = now + (entry.delivered ? 1000 : 100);
        }
    }
    for (auto it = legacy_.begin(); it != legacy_.end();) 
    {
        if (IsAccepted(send(*it))) 
            it = legacy_.erase(it);
        else 
            ++it;
    }
}