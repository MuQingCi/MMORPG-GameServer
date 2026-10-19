#include "test_util.h"
#include "db/taskDelivery.h"
#include "db/dbValue.h"

namespace {
Msg Request(uint64_t id) {
    Msg m;
    m.head.taskId = id;
    m.head.msgType = MSGTYPE_DB_TASK_MYSQL;
    m.head.playerId = 42;
    m.body = "fixed sql payload";
    return m;
}
Msg Result(const Msg& request) {
    Msg result = request;
    result.head.msgType = MSGTYPE_DB_RESULT_MYSQL;
    result.body = "execution result";
    return result;
}
}

TEST(TaskDeliveryRegistersBeforeDispatchAndRetriesOnlyAdmission) {
    PendingDbTasks tasks;
    auto request = Request(101);
    int calls = 0;
    auto blocked = [&](const Msg& m) {
        ++calls;
        CHECK(tasks.find(m.head.taskId) != nullptr);
        CHECK_EQ(m.body, request.body);
        return DispatchResult::kBackpressure;
    };
    CHECK(tasks.submit(request, 0, blocked));
    CHECK_EQ(tasks.find(101)->state, PendingDbTasks::State::PendingDispatch);
    tasks.tick(99, blocked);
    CHECK_EQ(calls, 1);
    tasks.tick(100, [&](const Msg&) { ++calls; return DispatchResult::kAccepted; });
    CHECK_EQ(tasks.find(101)->state, PendingDbTasks::State::AwaitResult);
    tasks.tick(100000, blocked);
    CHECK_EQ(calls, 2); // 超时不能盲目重试执行。
    auto wrong = Result(request); wrong.head.epoch = 2;
    CHECK(!tasks.complete(wrong));
    CHECK(tasks.complete(Result(request)));
    CHECK(!tasks.complete(Result(request)));
    CHECK(tasks.submit(request, 200, blocked));
    CHECK_EQ(calls, 2);
    request.body = "changed";
    CHECK(!tasks.submit(request, 200, blocked));
}

TEST(TaskDeliveryRejectsInvalidRoutesAndGeneratesIdentity) {
    PendingDbTasks tasks;
    CHECK(!tasks.submit(Request(1), 0, [](const Msg&) { return DispatchResult::kInvalidTarget; }));
    CHECK_EQ(tasks.size(), 0u);
    uint64_t first = 0, second = 0;
    CHECK(tasks.submit(Request(0), 0, [&](const Msg& m) { first=m.head.taskId; return DispatchResult::kAccepted; }));
    CHECK(tasks.submit(Request(0), 0, [&](const Msg& m) { second=m.head.taskId; return DispatchResult::kAccepted; }));
    CHECK(first != 0 && second != 0 && first != second);
    MsgBus bus(1);
    CHECK_EQ(bus.sendToWorker(2, {}), DispatchResult::kInvalidTarget);
    CHECK_EQ(bus.sendToDb({}), DispatchResult::kInvalidMessage);
    CHECK_EQ(bus.sendToWorker(0, {}), DispatchResult::kAccepted);
    auto broadcast = bus.broadcastToLogic({});
    CHECK_EQ(broadcast.size(), 1u);
    CHECK_EQ(broadcast[0], DispatchResult::kAccepted);
}

TEST(DbResultLedgerRetriesResultsWithoutReexecution) {
    DbResultLedger ledger;
    auto request = Request(202);
    CHECK_EQ(ledger.accept(request), DbResultLedger::Admission::New);
    ledger.finish(request, Result(request));
    int sends = 0;
    ledger.flush([&](const Msg& m) {
        ++sends; CHECK_EQ(m.head.taskId, 202u);
        return DispatchResult::kBackpressure;
    });
    ledger.flush([&](const Msg&) { ++sends; return DispatchResult::kAccepted; }, NowMs()+100);
    CHECK_EQ(sends, 2);
    ledger.flush([&](const Msg&) { ++sends; return DispatchResult::kAccepted; }, NowMs()+101);
    CHECK_EQ(sends, 2);
    CHECK_EQ(ledger.accept(request), DbResultLedger::Admission::Duplicate);
    ledger.flush([&](const Msg&) { ++sends; return DispatchResult::kAccepted; });
    CHECK_EQ(sends, 3);
    request.body = "different";
    CHECK_EQ(ledger.accept(request), DbResultLedger::Admission::Conflict);
}

TEST(DbResultAckLossAndStatusDoNotReexecute)
{
    PendingDbTasks tasks; DbResultLedger ledger;
    auto request = Request(303);
    CHECK(tasks.submit(request, 0, [](const Msg&) { return DispatchResult::kAccepted; }));
    CHECK_EQ(ledger.accept(request), DbResultLedger::Admission::New);
    CHECK_EQ(ledger.status(303),1);
    ledger.finish(request, Result(request));
    int sends = 0;
    ledger.flush([&](const Msg& result) { ++sends; CHECK(tasks.complete(result)); return DispatchResult::kAccepted; }, 0);
    CHECK_EQ(sends,1);
    // ACK 已被传递层接受，但在接收方处理前丢失。
    tasks.tickControls(0, [](const Msg& control) {
        CHECK_EQ(control.head.dbControl,1);
        return DispatchResult::kAccepted;
    });
    ledger.flush([&](const Msg&) { ++sends; return DispatchResult::kAccepted; },999);
    CHECK_EQ(sends,1);
    ledger.flush([&](const Msg& result) { ++sends; CHECK(tasks.acknowledgeDuplicate(result)); return DispatchResult::kAccepted; },1000);
    tasks.tickControls(1000, [&](const Msg& control) {
        CHECK(ledger.handleControl(control)); return DispatchResult::kAccepted;
    });
    ledger.flush([&](const Msg&) { ++sends; return DispatchResult::kAccepted; },2000);
    CHECK_EQ(sends,2);
    CHECK_EQ(ledger.status(303),2);
    auto forged = PendingDbTasks::control(request,1); forged.head.srcWorkerId=9;
    CHECK(!ledger.handleControl(forged));
    auto query = PendingDbTasks::control(request,2);
    CHECK(ledger.handleControl(query));
    ledger.flush([&](const Msg&) { ++sends; return DispatchResult::kAccepted; },2001);
    CHECK_EQ(sends,3);
    CHECK_EQ(ledger.accept(request),DbResultLedger::Admission::Duplicate);
}

TEST(DbControlQueuesAndUnknownStatus)
{
    MsgBus bus(1,2); auto request=Request(404);
    auto control=PendingDbTasks::control(request,2);
    CHECK_EQ(bus.sendToDbControl(control),DispatchResult::kAccepted);
    Msg received;
    CHECK(!bus.tryPopDbMySql(received));
    CHECK(bus.tryPopDbControl(false,0,received));
    CHECK_EQ(received.head.taskId,404u);
    control.head.msgType=MSGTYPE_DB_CONTROL_REDIS; control.head.dbShard=1;
    CHECK_EQ(bus.sendToDbControl(control),DispatchResult::kAccepted);
    CHECK(!bus.tryPopDbControl(true,0,received));
    CHECK(bus.tryPopDbControl(true,1,received));
    control.head.dbShard=2;
    CHECK_EQ(bus.sendToDbControl(control),DispatchResult::kInvalidTarget);
    PendingDbTasks tasks;
    CHECK(tasks.submit(request,0,[](const Msg&) { return DispatchResult::kAccepted; }));
    int queries=0;
    tasks.tickControls(5000,[&](const Msg& query) { ++queries; CHECK_EQ(query.head.dbControl,2); return DispatchResult::kAccepted; });
    CHECK_EQ(queries,1);
    Msg status=request; status.head.msgType=MSGTYPE_DB_STATUS; status.body.assign(1,0);
    CHECK(tasks.observeStatus(status));
    CHECK_EQ(tasks.find(404)->state,PendingDbTasks::State::AwaitResult);
    CHECK_EQ(tasks.find(404)->observedStatus,0);
    status.head.epoch=8;
    CHECK(!tasks.observeStatus(status));
}

TEST(DbResultClassificationWireCompatibility)
{
    dbv::DbResult result, decoded;
    result.errcode=7; result.errmsg="conflict";
    result.classification=dbv::ResultClass::kIdentityConflict;
    auto wire=result.Encode();
    CHECK(dbv::DbResult::Decode(wire,decoded));
    CHECK_EQ(decoded.classification,dbv::ResultClass::kIdentityConflict);
    wire.resize(wire.size()-37); // v1 不包含结果分类或元数据。
    wire[2] = 1;
    CHECK(dbv::DbResult::Decode(wire,decoded));
    CHECK_EQ(decoded.classification,dbv::ResultClass::kFailure);
    wire.push_back(char(255));
    wire[2] = 2;
    CHECK(!dbv::DbResult::Decode(wire,decoded));
}

TEST(DbLedgerCapacityRequiresAppliedAckNotQueueAdmission)
{
    DbResultLedger ledger(1); auto request=Request(505);
    CHECK(ledger.hasCapacity());
    CHECK_EQ(ledger.accept(request),DbResultLedger::Admission::New);
    ledger.finish(request,Result(request));
    ledger.flush([](const Msg&) { return DispatchResult::kAccepted; },0);
    CHECK(!ledger.hasCapacity()); // 结果仅进入 worker 队列，尚未处理。
    CHECK(ledger.handleControl(PendingDbTasks::control(request,1)));
    CHECK(ledger.hasCapacity());
    CHECK_EQ(ledger.status(505),0);
    CHECK(!ledger.handleControl(PendingDbTasks::control(request,2)));
}

TEST(ActiveTaskIndexExcludesHistoryAndPreservesAckRetry)
{
    PendingDbTasks tasks;
    tasks.enableActiveIndex();
    auto accepted=[](const Msg&){return DispatchResult::kAccepted;};
    auto blocked=[](const Msg&){return DispatchResult::kBackpressure;};
    for(uint64_t id=1;id<=512;++id) {
        const auto request=Request(id);
        CHECK(tasks.submit(request,0,accepted)); CHECK(tasks.complete(Result(request)));
    }
    tasks.tickControls(0,accepted);
    CHECK_EQ(tasks.size(),512u); CHECK_EQ(tasks.diagnosticSnapshot()[4],512u);
    PendingDbTasks::ScanProbe probe;
    tasks.tick(100,accepted,&probe);
    CHECK_EQ(probe.visited[3],0u); CHECK_EQ(probe.callbacks,0u);
    CHECK(tasks.acknowledgeDuplicate(Result(Request(10))));
    probe={};tasks.tickControls(100,blocked,&probe);
    CHECK_EQ(probe.visited[2],1u); CHECK_EQ(probe.callbacks,1u);
    probe={};tasks.tickControls(199,accepted,&probe); CHECK_EQ(probe.callbacks,0u);
    probe={};tasks.tickControls(200,accepted,&probe); CHECK_EQ(probe.callbacks,1u);
    probe={};tasks.tickControls(201,accepted,&probe); CHECK_EQ(probe.visited[3],0u);
    auto request=Request(999);
    CHECK(tasks.submit(request,0,blocked));
    tasks.tick(100,accepted);
    CHECK_EQ(tasks.find(999)->state,PendingDbTasks::State::AwaitResult);
    int execution=0;
    tasks.tick(10000,[&](const Msg&){++execution;return DispatchResult::kAccepted;});
    CHECK_EQ(execution,0);
    probe={};tasks.tickControls(5100,accepted,&probe); CHECK_EQ(probe.callbacks,1u);
    auto changed=Request(10);changed.body="conflict";CHECK(!tasks.submit(changed,0,accepted));
    auto forged=Result(Request(10));forged.body="forged result";
    CHECK(!tasks.acknowledgeDuplicate(forged));
}

TEST(ActiveTaskIndexMatchesLegacyTransitions)
{
    PendingDbTasks legacy,indexed;indexed.enableActiveIndex();
    std::vector<Msg> left,right;
    auto sender=[](std::vector<Msg>& out) {
        return [&out](const Msg& m) {
            out.push_back(m);
            if(m.head.taskId%7==0) return DispatchResult::kInvalidTarget;
            return m.head.taskId%3==0 ? DispatchResult::kBackpressure : DispatchResult::kAccepted;
        };
    };
    auto a=sender(left),b=sender(right);
    for(uint64_t id=1;id<=200;++id) CHECK_EQ(legacy.submit(Request(id),0,a),indexed.submit(Request(id),0,b));
    for(int64_t now=0;now<=10000;now+=100) {
        for(uint64_t id=1;id<=200;++id) {
            if(now==200 && id%2) CHECK_EQ(legacy.complete(Result(Request(id))),indexed.complete(Result(Request(id))));
            if(now==500 && id%2) CHECK_EQ(legacy.acknowledgeDuplicate(Result(Request(id))),indexed.acknowledgeDuplicate(Result(Request(id))));
        }
        left.clear();right.clear();
        legacy.tick(now,a);indexed.tick(now,b);
        legacy.tickControls(now,a);indexed.tickControls(now,b);
        auto order=[](const Msg& x,const Msg& y) {
            return std::pair(x.head.taskId,x.head.dbControl)<std::pair(y.head.taskId,y.head.dbControl);
        };
        std::sort(left.begin(),left.end(),order);std::sort(right.begin(),right.end(),order);
        CHECK_EQ(left.size(),right.size());
        for(size_t i=0;i<std::min(left.size(),right.size());++i) {
            CHECK_EQ(left[i].head.taskId,right[i].head.taskId);CHECK_EQ(left[i].head.dbControl,right[i].head.dbControl);
        }
        CHECK_EQ(legacy.diagnosticSnapshot(),indexed.diagnosticSnapshot());
        CHECK_EQ(legacy.unsettled(),indexed.unsettled());
    }
}

TEST(ActiveTaskIndexRedisAndCapacityReclamation)
{
    PendingDbTasks tasks; tasks.enableActiveIndex();
    const auto accepted=[](const Msg&) {return DispatchResult::kAccepted;};
    const auto blocked=[](const Msg&) {return DispatchResult::kBackpressure;};
    for(uint64_t id=1;id<=PendingDbTasks::kMaxTasks;++id) {
        auto request=Request(id);request.head.msgType=MSGTYPE_DB_TASK_REDIS;request.head.dbShard=1;
        auto result=request;result.head.msgType=MSGTYPE_DB_RESULT_REDIS;
        CHECK(tasks.submit(request,0,accepted));CHECK(tasks.complete(result));
    }
    tasks.tickControls(0,accepted);
    auto request=Request(PendingDbTasks::kMaxTasks+1);
    request.head.msgType=MSGTYPE_DB_TASK_REDIS;request.head.dbShard=1;
    CHECK(!tasks.submit(request,0,blocked)); // 历史记录必须保留 60 秒安全窗口。
    const auto now=NowMs()+60001;
    const bool submitted=tasks.submit(request,now,blocked);
    CHECK(submitted);
    if(!submitted) return;
    CHECK_EQ(tasks.size(),PendingDbTasks::kMaxTasks);
    tasks.tick(now+100,accepted);
    CHECK_EQ(tasks.find(request.head.taskId)->state,PendingDbTasks::State::AwaitResult);
    auto result=request;result.head.msgType=MSGTYPE_DB_RESULT_REDIS;
    CHECK(tasks.complete(result));
    PendingDbTasks::ScanProbe probe;
    tasks.tickControls(now+100,accepted,&probe);CHECK_EQ(probe.callbacks,1u);
    probe={};tasks.tick(now+200,accepted,&probe);CHECK_EQ(probe.visited[3],0u);
    CHECK(tasks.acknowledgeDuplicate(result));
    probe={};tasks.tickControls(now+200,accepted,&probe);CHECK_EQ(probe.callbacks,1u);
}

TEST(TaskSchedulingPolicyIsExplicitAndRejectsLateSwitch)
{
    using Policy=PendingDbTasks::SchedulingPolicy;
    PendingDbTasks legacy;
    CHECK_EQ(legacy.schedulingPolicy(),Policy::LegacyScan);
    PendingDbTasks indexed(Policy::ActiveIndex);
    CHECK_EQ(indexed.schedulingPolicy(),Policy::ActiveIndex);
    const auto accepted=[](const Msg&) {return DispatchResult::kAccepted;};
    CHECK(legacy.submit(Request(900),0,accepted));
    CHECK(!legacy.enableActiveIndex());
    CHECK_EQ(legacy.schedulingPolicy(),Policy::LegacyScan);
    CHECK(indexed.submit(Request(901),0,accepted));
    CHECK(!indexed.enableActiveIndex());
    CHECK_EQ(indexed.schedulingPolicy(),Policy::ActiveIndex);
}

TEST(ActiveIndexRemovesRetryFailureWithoutDiscardingIdentity)
{
    PendingDbTasks tasks;tasks.enableActiveIndex();
    const auto blocked=[](const Msg&) {return DispatchResult::kBackpressure;};
    const auto invalid=[](const Msg&) {return DispatchResult::kInvalidTarget;};
    CHECK(!tasks.submit(Request(910),0,invalid));
    CHECK_EQ(tasks.size(),0u);
    CHECK(tasks.submit(Request(911),0,blocked));
    tasks.tick(100,invalid);
    PendingDbTasks::ScanProbe probe;
    tasks.tick(200,invalid,&probe);
    CHECK_EQ(probe.visited[4],0u);
    CHECK_EQ(tasks.size(),1u);
    CHECK_EQ(tasks.find(911)->state,PendingDbTasks::State::DispatchFailed);
    CHECK(tasks.unsettled()); // 保留现有关闭语义。
}