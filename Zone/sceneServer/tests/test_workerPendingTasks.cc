#include "test_util.h"
#include "net/workerPendingTasks.h"
#include "gameProto.pb.h"
#include "net/net_server.h"
#include <sys/socket.h>
#include <unistd.h>
#include <thread>

namespace {
Msg Task(uint64_t id, uint64_t session = 10) {
    Msg m; m.head.playerId=id; m.head.session=session; m.head.seq=id;
    m.body="payload"; return m;
}
}

TEST(PublicErrorsPreserveWireValues)
{
    CHECK_EQ(gs::ERROR_CODE_ROUTE_NOT_FOUND,1001);
    CHECK_EQ(gs::ERROR_CODE_OPERATION_REJECTED,1008);
    CHECK_EQ(gs::ERROR_CODE_SERVER_BUSY,1011);
    CHECK_EQ(gs::ERROR_CODE_PENDING_TIMEOUT,1012);
    gs::RetTip tip; tip.set_code(gs::ERROR_CODE_SERVER_BUSY);tip.set_text("server busy");
    gs::RetTip decoded; CHECK(decoded.ParseFromString(tip.SerializeAsString()));
    CHECK_EQ(decoded.code(),1011);
}

TEST(WorkerPendingIsolatesAndPreservesFifo)
{
    WorkerPendingTasks pending(2,{});
    std::vector<uint64_t> delivered;
    bool blocked=true;
    auto send=[&](size_t worker,const Msg& m) {
        if(worker==0 && blocked) return DispatchResult::kBackpressure;
        delivered.push_back(m.head.seq);return DispatchResult::kAccepted;
    };
    CHECK_EQ(pending.submit(0,Task(1),true,0,send),WorkerPendingTasks::Admission::Pending);
    CHECK_EQ(pending.submit(0,Task(2),false,1,send),WorkerPendingTasks::Admission::Pending);
    CHECK_EQ(pending.submit(1,Task(3),false,1,send),WorkerPendingTasks::Admission::Accepted);
    CHECK(pending.pump(2,send,[](const Msg&){return true;}).empty());
    CHECK_EQ(delivered.size(),1u);
    blocked=false;pending.pump(3,send,[](const Msg&){return true;});
    CHECK_EQ(delivered.size(),3u);CHECK_EQ(delivered[1],1u);CHECK_EQ(delivered[2],2u);
    CHECK(pending.empty());
}

TEST(WorkerPendingDirectBusyLimitsExpiryAndCancellation)
{
    WorkerPendingTasks::Limits limits;limits.maxBusinessTasks=1;limits.businessTimeoutMs=1;
    WorkerPendingTasks pending(2,limits);
    auto blocked=[](size_t,const Msg&){return DispatchResult::kBackpressure;};
    CHECK_EQ(pending.submit(0,Task(1),false,0,blocked),WorkerPendingTasks::Admission::Busy);
    CHECK(pending.empty());
    pending.submit(0,Task(2),true,0,blocked);
    pending.submit(0,Task(3),false,0,blocked);
    CHECK_EQ(pending.submit(0,Task(4),false,0,blocked),WorkerPendingTasks::Admission::Busy);
    auto expired=pending.pump(1000,blocked,[](const Msg&){return true;});
    CHECK_EQ(expired.size(),1u);CHECK_EQ(expired[0].code,gs::ERROR_CODE_PENDING_TIMEOUT);
    CHECK_EQ(pending.stats(0).businessTasks,0u);CHECK_EQ(pending.stats(0).lifecycleTasks,1u);
    pending.submit(0,Task(5,20),false,1001,blocked);
    pending.cancelSession(20);
    CHECK_EQ(pending.stats(0).businessTasks,0u);
    CHECK_EQ(pending.stats(0).lifecycleTasks,1u);
    CHECK(pending.pump(5000,blocked,[](const Msg&){return true;}).empty());
}

TEST(WorkerPendingByteLimitStaleAndRoundRobinBudget)
{
    WorkerPendingTasks::Limits limits;limits.maxBusinessBytes=1;
    limits.perWorkerAttempts=1;limits.globalAttempts=1;
    WorkerPendingTasks pending(2,limits);
    auto blocked=[](size_t,const Msg&){return DispatchResult::kBackpressure;};
    pending.submit(0,Task(1),true,0,blocked);pending.submit(1,Task(2),true,0,blocked);
    CHECK_EQ(pending.submit(0,Task(3),false,0,blocked),WorkerPendingTasks::Admission::Busy);
    std::vector<size_t> workers;
    auto send=[&](size_t w,const Msg&){workers.push_back(w);return DispatchResult::kAccepted;};
    pending.pump(1,send,[](const Msg&){return true;});
    pending.pump(2,send,[](const Msg&){return true;});
    CHECK_EQ(workers.size(),2u);CHECK(workers[0]!=workers[1]);
    limits.maxBusinessBytes=1024;WorkerPendingTasks stale(1,limits);
    stale.submit(0,Task(1),true,0,blocked);stale.submit(0,Task(2),false,0,blocked);
    auto rejected=stale.pump(1,blocked,[](const Msg&){return false;});
    CHECK_EQ(rejected.size(),1u);CHECK_EQ(rejected[0].code,gs::ERROR_CODE_PLAYER_SESSION_INVALID);
}

TEST(WorkerPendingLifecycleSurvivesUnexpectedAdmissionFailure)
{
    WorkerPendingTasks pending(1,{});
    auto invalid=[](size_t,const Msg&){return DispatchResult::kInvalidTarget;};
    CHECK_EQ(pending.submit(0,Task(1),true,0,invalid),WorkerPendingTasks::Admission::Pending);
    CHECK_EQ(pending.stats(0).lifecycleTasks,1u);
    pending.pump(1000,invalid,[](const Msg&){return false;});
    CHECK_EQ(pending.stats(0).lifecycleTasks,1u);
}

// 在同一个所属线程中运行真实的 onFrame/信封/MsgBus/sendRetTip 套接字写入，
// 避免依赖时序的 epoll 竞态，也不人为设置总线容量。
struct NetServerTestPeer {
    static void exercise() {
        MsgBus bus(2);bus.enableBusyDiagnostics();NetConfig config;config.pending.businessTimeoutMs=1;
        NetServer net(config,bus);
        int sockets[2];CHECK_EQ(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,sockets),0);
        NetServer::Conn conn;conn.fd=sockets[0];conn.session=10;conn.gatewayId=1;
        conn.peerServiceID=ServerID::kGateway;net.conns_.emplace(10,std::move(conn));
        auto& connection=net.conns_.at(10);
        ClientData binding{100,200,1,1,1};
        const auto worker=bus.workerForPlayerId(binding.playerId);
        // 不启动工作线程消费者，以确定性方式耗尽现有队列。
        for(size_t n=0;n<100000;++n) {
            Msg filler;filler.head.msgType=MSGTYPE_CONN_DATA;
            if(!IsAccepted(bus.sendToWorker(worker,std::move(filler)))) break;
        }
        CHECK(bus.depth(worker)>0);
        net.clients_.Bind(10,binding);
        auto request=MakeHeader(kGatewayMsg,3,2,41,binding.playerId,kGateway,kScene);
        net.onFrame(connection,request,GatewayPlayer::Pack(binding,"business"));
        auto readReply=[&](int code,uint64_t seq) {
            char bytes[1024];const auto count=::recv(sockets[1],bytes,sizeof(bytes),0);
            CHECK(count>0);if(count<=0)return;
            Buffer buffer;buffer.append(bytes,count);Header reply;std::string body;
            CHECK_EQ(DecodeFrame(buffer,reply,kGateway,body),DecodeStatus::kOk);
            CHECK_EQ(reply.module,3);CHECK_EQ(reply.method,2);CHECK_EQ(reply.seq,seq);
            CHECK_EQ(reply.playerId,binding.playerId);
            gs::RetTip tip;CHECK(tip.ParseFromString(body));CHECK_EQ(tip.code(),code);
        };
        readReply(gs::ERROR_CODE_SERVER_BUSY,41);
        const auto evidence=bus.busyDiagnostics()->collect();
        CHECK_EQ(evidence.events.size(),1u);
        CHECK_EQ(evidence.events[0].worker,worker);
        CHECK_EQ(evidence.events[0].reason,0u);
        CHECK_EQ(evidence.events[0].seq,41u);
        CHECK_EQ(evidence.events[0].clientSession,200u);
        // 必须保留 ONLINE，业务只能排在其后。
        auto online=MakeHeader(kGatewayMsg,GatewayPlayer::kModule,GatewayPlayer::kOnline,0,binding.playerId,kGateway,kScene);
        net.onFrame(connection,online,GatewayPlayer::Pack(binding,{}));
        request.seq=42;net.onFrame(connection,request,GatewayPlayer::Pack(binding,"expires"));
        CHECK_EQ(net.pendingTasks_.stats(worker).businessTasks,1u);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        net.retryClientNotifications();readReply(gs::ERROR_CODE_PENDING_TIMEOUT,42);
        CHECK(bus.busyDiagnostics()->collect().events.empty());
        CHECK_EQ(net.pendingTasks_.stats(worker).businessTasks,0u);
        // 第二个工作线程仍能接收无关玩家的请求。
        auto other=binding;do {++other.playerId;} while(bus.workerForPlayerId(other.playerId)==worker);
        other.clientSession=201;net.clients_.Bind(10,other);
        request.playerId=other.playerId;request.seq=43;
        net.onFrame(connection,request,GatewayPlayer::Pack(other,"independent"));
        Msg received;CHECK(bus.tryPopWorker(bus.workerForPlayerId(other.playerId),received));
        CHECK_EQ(received.head.seq,43u);
        // 关闭连接会清除业务任务，但保留生命周期任务并生成 OFFLINE。
        request.playerId=binding.playerId;request.seq=44;
        net.onFrame(connection,request,GatewayPlayer::Pack(binding,"cancel"));
        net.handleClose(connection);
        CHECK_EQ(net.pendingTasks_.stats(worker).businessTasks,0u);
        CHECK_EQ(net.pendingTasks_.stats(worker).lifecycleTasks,2u);
        ::close(sockets[1]);
    }
};

TEST(NetPendingCorrelatedBusyTimeoutAndWorkerIsolation) { NetServerTestPeer::exercise(); }

TEST(WorkerPendingOnlineBusinessOfflineOrderAndNoLateExecution)
{
    WorkerPendingTasks pending(1,{});
    auto blocked=[](size_t,const Msg&){return DispatchResult::kBackpressure;};
    pending.submit(0,Task(1),true,0,blocked);
    pending.submit(0,Task(2),false,0,blocked);
    pending.submit(0,Task(3),true,0,blocked);
    std::vector<uint64_t> delivered;
    auto send=[&](size_t,const Msg& m){delivered.push_back(m.head.seq);return DispatchResult::kAccepted;};
    auto failed=pending.pump(500000,send,[](const Msg&){return true;});
    CHECK_EQ(failed.size(),1u);CHECK_EQ(failed[0].message.head.seq,2u);
    CHECK_EQ(delivered.size(),2u);CHECK_EQ(delivered[0],1u);CHECK_EQ(delivered[1],3u);
    CHECK(pending.pump(1000000,send,[](const Msg&){return true;}).empty());
    CHECK_EQ(delivered.size(),2u);CHECK_EQ(pending.waitMs(0),-1);
}