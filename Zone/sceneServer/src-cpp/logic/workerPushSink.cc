#include "logic/workerPushSink.h"
#include "common/msg.h"
#include "service/aoi/aoiPushSink.h"
#include "common/msgBus.h"
#include <utility>
#include <unordered_set>



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
    AoiWorkerPublishResult result;
    std::unordered_set<workerId> seen;
    const auto self = playerManager_.workerId();
    for(auto target : request.targets) 
    {
        if(!seen.insert(target).second) 
            continue;
        bool accepted = false;
        if(request.msgType == MSGTYPE_SERVICE && 
           request.module == Module::AOI &&
           target < msgBus_.numWorker() && 
           target != self) 
        {
            Msg msg;
            msg.head.msgType = MSGTYPE_SERVICE;
            msg.head.Module = request.module;
            msg.head.Method = request.method;
            msg.head.serviceSenderWorkerId = self;
            msg.body = request.body;
            accepted = msgBus_.sendToWorker(target, std::move(msg));
        }
        result.deliveries.push_back({target, accepted});
    }
    return result;
}
