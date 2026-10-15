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