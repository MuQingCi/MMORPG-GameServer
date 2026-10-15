#include "test_util.h"
#include "service/aoi/aoiServiceRouter.h"
#include "logic/workerPushSink.h"
#include <stdexcept>

namespace {
Map RoutingMap() {
    Map map; std::string err;
    if(!Map::Load(std::string(SCENE_SOURCE_DIR) + "/config/map.yaml",50,map,err))
        throw std::runtime_error(err);
    return map;
}
AoiEvent RoutingEvent() {
    AoiEvent e;
    e.msgKind = AoiMsgKind::kEntry; e.entityKind = EntityKind::kPlayer;
    e.eId = 123; e.businessId = 456; e.entityGeneration = 1;
    e.spatialVersion = 1; e.ownerSeq = 1; e.ownerThreadId = 0;
    e.target_x = 10; e.target_y = 10; e.target_dir = -3;
    return e;
}
}

TEST(AoiWireRejectsPartialAndTrailingData) {
    auto event = RoutingEvent(); std::string body;
    CHECK(AoiWire::EncodeEvent(event,body));
    CHECK_EQ(body.size(),AoiWire::kEventBytes);
    AoiEvent decoded;
    CHECK(AoiWire::DecodeEvent(body,decoded));
    CHECK_EQ(decoded.target_dir,-3);
    CHECK_EQ(decoded.ownerSeq,1u);
    for(size_t n = 0; n < body.size(); ++n)
        CHECK(!AoiWire::DecodeEvent(body.substr(0,n),decoded));
    CHECK(!AoiWire::DecodeEvent(body + "x",decoded));
    AoiEntity ent; updateEntity(ent,event,999);
    AoiOwnerSnapshot snap{1,0,{ent}}, out{};
    CHECK(AoiWire::EncodeSnapshot(snap,body));
    CHECK(AoiWire::DecodeSnapshot(body,out));
    CHECK_EQ(out.ents.size(),1u);
    CHECK_EQ(out.ents[0].cellIdx,0u);
    CHECK(!AoiWire::DecodeSnapshot(body.substr(0,body.size()-1),out));
    CHECK(!AoiWire::DecodeSnapshot(body + "x",out));
    snap.ents.push_back(ent);
    CHECK(!AoiWire::EncodeSnapshot(snap,body));
}

TEST(AoiInternalSinkAndRouterValidation) {
    MsgBus bus(2);
    PlayerManager players0(0), players1(1);
    WorkerPushSink sink0(bus,players0), sink1(bus,players1);
    auto map = RoutingMap(); AoiConfig cfg;
    AoiService receiver(1,cfg,sink1,map);
    const auto hooks = AoiServiceRouter::PublishHooks(0,2);
    CHECK_EQ(hooks.publishTargets,(std::vector<workerId>{1}));
    AoiWorkerPublishRequest request;
    CHECK(hooks.encodeEvent(RoutingEvent(),request));
    request.targets = {1,1,0,9};
    const auto result = sink0.publishToWorkers(request);
    CHECK_EQ(result.deliveries.size(),3u);
    CHECK(result.deliveries[0].accepted);
    CHECK(!result.deliveries[1].accepted);
    CHECK(!result.deliveries[2].accepted);
    Msg msg;
    CHECK(bus.tryPopWorker(1,msg));
    CHECK_EQ(msg.head.serviceSenderWorkerId,0u);
    CHECK_EQ(msg.head.msgType,uint16_t(MSGTYPE_SERVICE));
    CHECK_EQ(AoiServiceRouter::Dispatch(receiver,1,2,msg),AoiServiceRouter::Result::kHandled);
    msg.head.Method = Method::AOI_MOVE;
    CHECK_EQ(AoiServiceRouter::Dispatch(receiver,1,2,msg),AoiServiceRouter::Result::kRejected);
    msg.head.Method = Method::AOI_ENTRY;
    msg.head.serviceSenderWorkerId = 1;
    CHECK_EQ(AoiServiceRouter::Dispatch(receiver,1,2,msg),AoiServiceRouter::Result::kRejected);
    msg.head.serviceSenderWorkerId = 0;
    msg.head.msgType = MSGTYPE_CONN_DATA;
    CHECK_EQ(AoiServiceRouter::Dispatch(receiver,1,2,msg),AoiServiceRouter::Result::kRejected);
    msg.head.msgType = MSGTYPE_SERVICE;
    msg.head.Method = Method::AOI_OWNER_SNAPSHOT_REQ;
    msg.body = AoiWire::SnapshotRequest();
    CHECK_EQ(AoiServiceRouter::Dispatch(receiver,1,2,msg),AoiServiceRouter::Result::kSnapshotRequested);
    msg.head.Method = Method::AOI_OWNER_SNAPSHOT;
    CHECK(AoiWire::EncodeSnapshot(AoiOwnerSnapshot{2,0,{}},msg.body));
    CHECK_EQ(AoiServiceRouter::Dispatch(receiver,1,2,msg),AoiServiceRouter::Result::kHandled);
    CHECK_EQ(players1.size(),0u);
}