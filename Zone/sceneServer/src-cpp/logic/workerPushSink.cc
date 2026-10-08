#include "logic/workerPushSink.h"
#include "common/msg.h"
#include "service/aoi/aoiPushSink.h"
#include "common/msgBus.h"
#include <utility>



WorkerPushSink::WorkerPushSink(MsgBus& msgBus, PlayerManager& playerManager)
                             : msgBus_(msgBus),
                               playerManager_(playerManager)
{
}

WorkerPushSink::~WorkerPushSink() = default;

AoiPushResult WorkerPushSink::sendToClient(const AoiClientPushRequest& request)
{
    auto id = request.observerPlayerId;
    auto epoch = request.clientViewEpoch;
    
    auto playerPtr = playerManager_.get(id);
    if(playerPtr == nullptr)
        return AoiPushResult::kObserverOffline;
    if(epoch < playerPtr->epoch())
        return AoiPushResult::kSessionChanged;
    
    auto session = playerPtr->session();
    Msg msg;
    msg.head.playerId = playerPtr->id();
    msg.head.session = session;
    
    msg.head.Module = request.module;
    msg.head.Method = request.method;
    msg.body        = std::move(request.body);
    bool res = msgBus_.sendToNet(msg);
    if(!res)
        return AoiPushResult::kQueueFull;
    return AoiPushResult::kAccepted;
}

AoiWorkerPublishResult WorkerPushSink::publishToWorkers(const AoiWorkerPublishRequest& request)
{
    //TODO 使用MsgBus的broadcastToLogic/broadcastToLogicExcept来广播/投递到对应逻辑线程
    AoiWorkerPublishResult res;

    //...
    return res;
}
