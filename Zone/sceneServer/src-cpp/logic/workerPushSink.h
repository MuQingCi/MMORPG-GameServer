#ifndef CLEARMOON_SCENESERVER_LOGIC_WORKERPUSHSINK_H
#define CLEARMOON_SCENESERVER_LOGIC_WORKERPUSHSINK_H

#include "common/msgBus.h"
#include "player/playerManager.h"
#include "service/aoi/aoiPushSink.h"


class WorkerPushSink final : public AoiPushSink
{
public:
    WorkerPushSink(MsgBus& msgBus, PlayerManager& playerManager);
    ~WorkerPushSink();

    virtual AoiPushResult sendToClient(const AoiClientPushRequest& request);

    virtual AoiWorkerPublishResult publishToWorkers(const AoiWorkerPublishRequest& request);
private:
    MsgBus& msgBus_;
    const PlayerManager& playerManager_;
};

#endif