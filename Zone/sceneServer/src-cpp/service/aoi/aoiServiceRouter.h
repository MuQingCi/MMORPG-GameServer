#ifndef CLEARMOON_SCENE_AOI_SERVICE_ROUTER_H
#define CLEARMOON_SCENE_AOI_SERVICE_ROUTER_H

#include "service/aoi/aoiService.h"
#include "service/aoi/aoiWire.h"

namespace AoiServiceRouter {
enum class Result { kHandled, kRejected, kSnapshotRequested };

// 仅由内部 Service 分支调用；网络消息不能通过此路由成为远程 AOI 事件。
inline Result Dispatch(AoiService& service, workerId self, size_t workerCount, const Msg& msg)
{
    const auto sender = msg.head.serviceSenderWorkerId;
    if(msg.head.msgType != MSGTYPE_SERVICE || msg.head.Module != Module::AOI ||
       sender >= workerCount || sender == self)
        return Result::kRejected;

    switch(msg.head.Method) 
    {
    case Method::AOI_ENTRY:
    case Method::AOI_MOVE:
    case Method::AOI_LEAVE: 
    {
        AoiEvent event;
        if(!AoiWire::DecodeEvent(msg.body, event) || event.ownerThreadId != sender ||
           AoiWire::EventMethod(event.msgKind) != msg.head.Method)
            return Result::kRejected;
        return service.onRemoteEntityEvent(sender, event) == AoiService::RemoteResult::kRejected
            ? Result::kRejected : Result::kHandled;
    }
    case Method::AOI_OWNER_SNAPSHOT: 
    {
        AoiOwnerSnapshot snapshot{};
        if(!AoiWire::DecodeSnapshot(msg.body, snapshot) || snapshot.ownerWokerId != sender)
            return Result::kRejected;
        const auto result = service.ApplyOwnerSnapshot(sender, snapshot);
        return result == AoiService::ApplyResult::kApplied || result == AoiService::ApplyResult::kIgnored
            ? Result::kHandled : Result::kRejected;
    }
    case Method::AOI_OWNER_SNAPSHOT_REQ:
        return AoiWire::ValidSnapshotRequest(msg.body) ? Result::kSnapshotRequested : Result::kRejected;
    default:
        return Result::kRejected;
    }
}

inline AoiServiceHooks PublishHooks(workerId self, size_t workerCount)
{
    AoiServiceHooks hooks;
    for(size_t id = 0; id < workerCount; ++id)
        if(id != self) 
            hooks.publishTargets.push_back(static_cast<workerId>(id));

    hooks.encodeEvent = [](const AoiEvent& event, AoiWorkerPublishRequest& request)
    {
        request.msgType = MSGTYPE_SERVICE;
        request.module = Module::AOI;
        request.method = AoiWire::EventMethod(event.msgKind);
        return request.method != 0 && AoiWire::EncodeEvent(event, request.body);
    };
    return hooks;
}
}
#endif