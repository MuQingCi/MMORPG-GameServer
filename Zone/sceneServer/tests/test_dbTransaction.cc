#include "test_util.h"
#include "db/playerSnapshotDao.h"
#include "db/transactionExecutor.h"
#include "db/mysqlConnection.h"
#include "db/transactionRetries.h"

namespace {
struct FakeBackend : dbv::TransactionBackend {
    int begins=0, commits=0, rollbacks=0, calls=0, failAt=-1;
    int64_t storedVersion=0;
    bool rollbackOk=true, ambiguousCommit=false, badReplay=false;
    const dbv::DbTask* snapshot=nullptr;
    void begin(dbv::DbResult& result) override { ++begins; result={}; }
    void execute(const dbv::SqlStep& step, dbv::DbResult& result) override {
        result={}; ++calls;
        if (calls==failAt) {
            result.errcode=1003; result.nativeError=1213;
            result.classification=dbv::ResultClass::kRetryable; return;
        }
        if (step.args[0].find("SELECT save_version") == 0) {
            result.data=dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(storedVersion)})});
        } else if (step.checkData) {
            result.data=step.expectedData;
            if (badReplay) result.data=dbv::Val::Nil();
        } else {
            result.data=dbv::Val::Arr({dbv::Val::Int(1),dbv::Val::Int(0)});
        }
    }
    void commit(dbv::DbResult& result) override {
        ++commits; result={};
        if (ambiguousCommit) {
            result.errcode=1003; result.nativeError=2013;
            result.classification=dbv::ResultClass::kOutcomeUnknown;
        }
    }
    bool rollback() override { ++rollbacks; return rollbackOk; }
};
dbv::DbTask SnapshotTask() {
    PlayerSaveSnapshot snapshot;
    snapshot.playerId=42; snapshot.saveVersion=10;
    snapshot.capturedRevision=6; snapshot.saveLifecycleToken=8;
    snapshot.gold=30; snapshot.x=20; snapshot.bagNextSlot=2;
    snapshot.items={{7,2,1},{9,4,0}};
    dbv::DbTask task; std::string err;
    CHECK(PlayerSnapshotDao::buildSave(snapshot,task,err));
    return task;
}
}

TEST(TransactionProtocolRoundtripAndMalformedInput)
{
    auto task=SnapshotTask(); dbv::DbTask decoded;
    const auto body=task.Encode();
    CHECK(dbv::DbTask::Decode(body,decoded));
    CHECK_EQ(decoded.kind,5);
    CHECK_EQ(decoded.saveVersion,10u);
    CHECK_EQ(decoded.steps.size(),5u);
    CHECK_EQ(decoded.verification.size(),3u);
    CHECK_EQ(decoded.Encode(),body);
    for (size_t i=0; i<body.size(); ++i) CHECK(!dbv::DbTask::Decode(body.substr(0,i),decoded));
    CHECK(!dbv::DbTask::Decode(body+"x",decoded));
    dbv::DbTask legacy; legacy.kind=2; legacy.args={"UPDATE player SET hp=? WHERE id=?","9","42"};
    CHECK(dbv::DbTask::Decode(legacy.Encode(),decoded));
    CHECK_EQ(decoded.args,legacy.args);
    CHECK(decoded.steps.empty());
    dbv::DbResult result, loaded;
    result.nativeError=1213; result.playerId=42; result.saveVersion=10;
    result.capturedRevision=6; result.saveLifecycleToken=8;
    result.errcode=1003; result.classification=dbv::ResultClass::kRetryable;
    CHECK(dbv::DbResult::Decode(result.Encode(),loaded));
    CHECK_EQ(loaded.nativeError,1213u); CHECK_EQ(loaded.saveVersion,10u);
    CHECK_EQ(loaded.capturedRevision,6u); CHECK_EQ(loaded.saveLifecycleToken,8u);
}

TEST(PlayerLoadValidatesCompleteStateAndVersions)
{
    const auto task=PlayerSnapshotDao::buildLoad(42);
    CHECK_EQ(task.kind,4); CHECK_EQ(task.steps.size(),3u);
    CHECK(task.steps[0].args[0].find("FOR UPDATE")!=std::string::npos);
    dbv::DbResult result; result.playerId=42;
    result.data=dbv::Val::Arr({
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(90),dbv::Val::Int(40),dbv::Val::Int(99),
            dbv::Val::Int(10),dbv::Val::Int(20),dbv::Val::Int(1),dbv::Val::Int(1),dbv::Val::Int(8)})}),
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(64),dbv::Val::Int(2),dbv::Val::Int(8)})}),
        dbv::Val::Arr({dbv::Val::Arr({dbv::Val::Int(7),dbv::Val::Int(1),dbv::Val::Int(3)})})});
    PlayerSaveSnapshot snapshot; std::string error;
    CHECK(PlayerSnapshotDao::parseLoad(42,result,snapshot,error));
    CHECK_EQ(snapshot.saveVersion,8u); CHECK_EQ(snapshot.items[0].count,3);
    result.data.a[1].a[0].a[2]=dbv::Val::Int(7);
    CHECK(!PlayerSnapshotDao::parseLoad(42,result,snapshot,error));
    CHECK_EQ(snapshot.saveVersion,8u); // 失败不替换输出对象。
    result.data.a[0]=dbv::Val::Arr({});
    CHECK(!PlayerSnapshotDao::parseLoad(42,result,snapshot,error));
}

TEST(TransactionRollsBackFailureAndChecksBusinessConditions)
{
    auto task=SnapshotTask(); FakeBackend backend; backend.failAt=4;
    auto result=dbv::executeTransaction(task,backend);
    CHECK_EQ(result.classification,dbv::ResultClass::kRetryable);
    CHECK_EQ(result.nativeError,1213u); CHECK_EQ(backend.commits,0);
    CHECK_EQ(backend.rollbacks,1); CHECK_EQ(backend.calls,4);
    backend.failAt=-1;
    result=dbv::executeTransaction(task,backend); // 重试整个不变任务，而不只是失败步骤。
    CHECK_EQ(result.classification,dbv::ResultClass::kSuccess);
    CHECK_EQ(backend.commits,1); CHECK_EQ(backend.begins,2);
    CHECK_EQ(result.playerId,42u); CHECK_EQ(result.saveVersion,10u);
    task.steps[0].expectedAffected=2;
    result=dbv::executeTransaction(task,backend);
    CHECK_EQ(result.classification,dbv::ResultClass::kBusinessRejected);
    CHECK_EQ(backend.commits,1);
    backend.rollbackOk=false;
    result=dbv::executeTransaction(task,backend);
    CHECK_EQ(result.classification,dbv::ResultClass::kOutcomeUnknown);
}

TEST(SnapshotVersionProtectionAndReplayPayloadValidation)
{
    auto task=SnapshotTask(); FakeBackend backend;
    backend.storedVersion=11;
    auto result=dbv::executeTransaction(task,backend);
    CHECK_EQ(result.classification,dbv::ResultClass::kSuperseded);
    CHECK_EQ(backend.calls,1); CHECK_EQ(backend.commits,0);
    backend.storedVersion=10;
    result=dbv::executeTransaction(task,backend);
    CHECK_EQ(result.classification,dbv::ResultClass::kSuccess);
    CHECK_EQ(backend.calls,5); CHECK_EQ(backend.commits,0);
    backend.badReplay=true;
    result=dbv::executeTransaction(task,backend);
    CHECK_EQ(result.classification,dbv::ResultClass::kVersionConflict);
    CHECK_EQ(backend.commits,0);
    backend.storedVersion=0; backend.badReplay=false; backend.ambiguousCommit=true;
    result=dbv::executeTransaction(task,backend);
    CHECK_EQ(result.classification,dbv::ResultClass::kOutcomeUnknown);
    CHECK_EQ(result.nativeError,2013u);
}

TEST(SnapshotDaoValidationAndTransactionDdlRejection)
{
    auto task=SnapshotTask();
    CHECK_EQ(task.steps[3].args[2],"9"); // 按槽位排序形成规范顺序。
    CHECK_EQ(task.steps[4].args[2],"7");
    PlayerSaveSnapshot bad; bad.playerId=42; bad.saveVersion=1;
    bad.items={{1,1,0}}; bad.bagNextSlot=0;
    std::string err;
    const auto original=task.Encode();
    CHECK(!PlayerSnapshotDao::buildSave(bad,task,err));
    CHECK_EQ(task.Encode(),original);
    bad.bagNextSlot=1; bad.items.push_back(bad.items[0]);
    CHECK(!PlayerSnapshotDao::buildSave(bad,task,err));
    task.kind=4; task.verification.clear();
    task.steps[0].args={"TRUNCATE TABLE player"};
    FakeBackend backend;
    auto result=dbv::executeTransaction(task,backend);
    CHECK_EQ(result.classification,dbv::ResultClass::kFailure);
    CHECK_EQ(backend.begins,0);
    task.steps[0].args={"COMMIT"};
    CHECK(!dbv::validStep(task.steps[0]));
    task.steps[0].args={"UPDATE player SET hp=1; COMMIT"};
    CHECK(!dbv::validStep(task.steps[0]));
    MySqlConnection mysql;
    mysql.Execute(SnapshotTask(),result); // 未建立连接，不访问数据库。
    CHECK(result.errcode != 0);
    CHECK_EQ(result.playerId,42u);
}

TEST(TransactionRetryBudgetAndUnknownOutcomeBarrier)
{
    TransactionRetries retries;
    auto task=SnapshotTask(); Msg request; request.head.taskId=123;
    dbv::DbResult retry; retry.errcode=1003; retry.classification=dbv::ResultClass::kRetryable;
    CHECK(retries.schedule(request,task,retry,1,0));
    int executes=0, completions=0;
    auto execute=[&](const dbv::DbTask& fixed, dbv::DbResult& result) {
        ++executes; CHECK_EQ(fixed.Encode(),task.Encode()); result=retry;
    };
    auto complete=[&](const Msg& original,const dbv::DbResult& result) {
        ++completions; CHECK_EQ(original.head.taskId,123u);
        CHECK_EQ(result.classification,dbv::ResultClass::kRetryable);
    };
    retries.tick(99,execute,complete); CHECK_EQ(executes,0);
    retries.tick(100,execute,complete); CHECK_EQ(executes,1); CHECK_EQ(completions,0);
    retries.tick(299,execute,complete); CHECK_EQ(executes,1);
    retries.tick(300,execute,complete); CHECK_EQ(executes,2); CHECK_EQ(completions,1);
    CHECK_EQ(retries.size(),0u);
    retry.classification=dbv::ResultClass::kOutcomeUnknown;
    CHECK(!retries.schedule(request,task,retry,1,400));
    retry.classification=dbv::ResultClass::kFailure;
    CHECK(!retries.schedule(request,task,retry,1,400));
}