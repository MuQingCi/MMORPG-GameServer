#ifndef CLEARMOON_SCENE_PLAYER_SERVICE_H
#define CLEARMOON_SCENE_PLAYER_SERVICE_H

#include "player/playerManager.h"
#include "service/aoi/aoiService.h"

// 所有操作必须在 owner worker 执行。业务决定何时进场；Actor 创建不是进场。
class PlayerService
{
public:
    enum class Result { kOk, kNotFound, kNotInScene, kAlreadyInScene, kOutOfBounds, kAoiRejected };
    
    PlayerService(PlayerManager& players, AoiService& aoi, const Map& map);

    Result EnterScene(uint64_t pid, int32_t x, int32_t y, int32_t dir);
    Result Move(uint64_t pid, int32_t x, int32_t y, int32_t dir);
    Result LeaveScene(uint64_t pid);

private:
    PlayerManager& players_;
    AoiService& aoi_;
    const Map& map_;
};
#endif
