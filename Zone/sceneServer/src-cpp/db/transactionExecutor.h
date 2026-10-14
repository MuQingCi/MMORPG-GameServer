#ifndef SCENE_DB_TRANSACTION_EXECUTOR_H
#define SCENE_DB_TRANSACTION_EXECUTOR_H

#include "db/dbValue.h"
#include <algorithm>
#include <cctype>
#include <limits>

namespace dbv {
// 后端由所属线程独占。实现必须在同一连接上执行事务；
// 回滚或事务状态清理失败时必须丢弃连接。仅支持 InnoDB 的数据操作语句。
class TransactionBackend 
{
public:
    virtual ~TransactionBackend() = default;
    virtual void begin(DbResult& result) = 0;
    virtual void execute(const SqlStep& step, DbResult& result) = 0;
    virtual void commit(DbResult& result) = 0;
    virtual bool rollback() = 0;
};

inline bool equalValue(const Val& a, const Val& b) 
{
    if (a.kind != b.kind) 
        return false;

    if (a.IsInt()) 
        return a.i == b.i;

    if (a.IsStr()) 
        return a.s == b.s;

    if (a.IsArr()) 
    {
        if (a.a.size() != b.a.size()) 
            return false;
        for (size_t i = 0; i < a.a.size(); ++i) 
            if (!equalValue(a.a[i], b.a[i]))
                return false;
    }
    return true;
}

inline bool validStep(const SqlStep& step) 
{
    if (step.args.empty() || step.args[0].empty() || step.expectedAffected < -1 ||
        (step.kind != 1 && step.kind != 2) || (step.kind == 1 && step.expectedAffected != -1)) 
        return false;
    
    const auto& sql = step.args[0];
    const auto first = sql.find_first_not_of(" \t\r\n");

    if (first == std::string::npos || sql.find(';') != std::string::npos) 
        return false;

    auto end = first;
    while (end < sql.size() && std::isalpha(static_cast<unsigned char>(sql[end])))
        ++end;
    
    auto verb = sql.substr(first, end-first);
    std::transform(verb.begin(), verb.end(), verb.begin(), [](unsigned char c) { return std::toupper(c); });

    if (step.kind == 1) 
        return verb == "SELECT";
    return verb == "INSERT" || verb == "UPDATE" || verb == "DELETE";
}

inline DbResult executeTransaction(const DbTask& task, TransactionBackend& backend) 
{
    DbResult out;
    out.playerId = task.playerId; 
    out.saveVersion = task.saveVersion;
    out.capturedRevision = task.capturedRevision; 
    out.saveLifecycleToken = task.saveLifecycleToken;

    auto fail = [&](ResultClass kind, const char* message) {
        out.errcode = 1010; 
        out.errmsg = message; 
        out.classification = kind;
    };

    if ((task.kind != 4 && task.kind != 5) || task.steps.empty() || task.steps.size() > 65540 ||
        !task.args.empty() || (task.kind == 4 && !task.verification.empty())) 
    {
        fail(ResultClass::kFailure, "invalid transaction request"); 
        return out;
    }

    for (const auto& step : task.steps) 
        if (!validStep(step)) 
        {
            fail(ResultClass::kFailure, "transaction accepts parameterized DML only"); return out;
        }
    for (const auto& step : task.verification) 
        if (!validStep(step) || step.kind != 1 || !step.checkData) 
        {
            fail(ResultClass::kFailure, "invalid snapshot verification"); 
            return out;
        }
    
    if (task.kind == 5 && (!task.playerId || !task.saveVersion ||
        task.playerId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        task.saveVersion > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        task.verification.size() != 3)) 
    {
        fail(ResultClass::kFailure, "invalid snapshot identity/version"); 
        return out;
    }
    auto copyError = [&](const DbResult& result) {
        out.errcode = result.errcode; 
        out.errmsg = result.errmsg;
        out.classification = result.classification; 
        out.nativeError = result.nativeError;
        if (out.classification == ResultClass::kSuccess) 
            out.classification = ResultClass::kFailure;
    };
    auto abort = [&]() {
        if (!backend.rollback()) 
        {
            out.errcode = 1011; 
            out.errmsg += "; rollback not confirmed";
            out.classification = ResultClass::kOutcomeUnknown;
        }
    };
    
    DbResult result;
    backend.begin(result);
    if (result.errcode) 
    { 
        copyError(result); 
        return out; 
    }
    try {
        if (task.kind == 5) 
        {
            SqlStep lock;
            lock.kind = 1;
            lock.args = {"SELECT save_version FROM player WHERE id=? FOR UPDATE", std::to_string(task.playerId)};
            backend.execute(lock, result);
            if (result.errcode) 
            { 
                copyError(result); 
                abort(); 
                return out; 
            }
            if (!result.data.IsArr() || result.data.a.size() != 1 ||
                !result.data.a[0].IsArr() || result.data.a[0].a.size() != 1 ||
                !result.data.a[0].a[0].IsInt() || result.data.a[0].a[0].i < 0) 
            {
                fail(ResultClass::kVersionConflict, "player row missing or invalid; creation requires explicit provisioning");
                abort(); 
                return out;
            }
            const auto version = static_cast<uint64_t>(result.data.a[0].a[0].i);
            if (version > task.saveVersion) 
            {
                out.classification = ResultClass::kSuperseded;
                abort(); 
                return out;
            }
            if (version == task.saveVersion) 
            {
                for (const auto& step : task.verification) 
                {
                    backend.execute(step, result);
                    if (result.errcode) 
                    { 
                        copyError(result); 
                        abort(); 
                        return out; 
                    }
                    if (!equalValue(result.data, step.expectedData)) 
                    {
                        fail(ResultClass::kVersionConflict, "same save_version has different persistent payload");
                        abort(); 
                        return out;
                    }
                }
                abort(); // 只读重放，不执行可能产生未知结果的提交。
                return out;
            }
        }
        std::vector<Val> results;
        for (const auto& step : task.steps) 
        {
            backend.execute(step, result);
            if (result.errcode) 
            { 
                copyError(result); 
                abort(); 
                return out; 
            }

            if (step.expectedAffected >= 0 && (!result.data.IsArr() || result.data.a.empty() ||
                !result.data.a[0].IsInt() || result.data.a[0].i != step.expectedAffected)) 
            {
                fail(ResultClass::kBusinessRejected, "affected row precondition failed"); 
                abort(); 
                return out;
            }
            if (step.checkData && !equalValue(result.data, step.expectedData)) 
            {
                fail(ResultClass::kBusinessRejected, "SQL result precondition failed"); 
                abort(); 
                return out;
            }
            results.push_back(result.data);
        }
        backend.commit(result);
        if (result.errcode) 
        { 
            copyError(result); 
            abort(); 
            return out; 
        }
        out.data = Val::Arr(std::move(results));
        return out;
    } catch (...) {
        backend.rollback();
        throw;
    }
}
} // namespace dbv
#endif