#ifndef CLEARMOON_SCENE_PLAYER_SERVICE_H
#define CLEARMOON_SCENE_PLAYER_SERVICE_H

#include "player/playerManager.h"
#include "service/aoi/aoiService.h"

// 所有操作必须在 owner worker 执行。业务决定何时进场；Actor 创建不是进场。
class PlayerService
{
public:
    enum class Result { kOk, kNotFound, kNotInScene, kAlreadyInScene, kOutOfBounds, kAoiRejected };
    PlayerService(PlayerManager& players, AoiService& aoi, const Map& map)
        : players_(players), aoi_(aoi), map_(map) {}

    Result EnterScene(uint64_t pid, int32_t x, int32_t y, int32_t dir)
    {
        auto* p = players_.get(pid);
        if(!p) return Result::kNotFound;
        if(!map_.contains(x,y)) return Result::kOutOfBounds;
        const auto id = aoi_.EntityOf(pid);
        if(id && aoi_.IsEntityActive(*id)) return Result::kAlreadyInScene;
        const auto oldX = p->x(), oldY = p->y(), oldDir = p->dir();
        const bool dirty = p->dirty();
        p->setPos(x,y,dir);
        const auto result = aoi_.OnEntityEnter(*p);
        if(result != AoiService::ApplyResult::kApplied) {
            p->setPos(oldX,oldY,oldDir);
            if(!dirty) p->clearDirty();
            return Result::kAoiRejected;
        }
        const auto& binding=p->clientBinding();
        if(GatewayPlayer::Valid(binding))
            aoi_.OnPlayerOnline(pid,binding.gatewayId,binding.clientEpoch);
        return Result::kOk;
    }

    Result Move(uint64_t pid, int32_t x, int32_t y, int32_t dir)
    {
        auto* p = players_.get(pid);
        if(!p) return Result::kNotFound;
        if(!map_.contains(x,y)) return Result::kOutOfBounds;
        const auto oldX = p->x(), oldY = p->y(), oldDir = p->dir();
        const bool dirty = p->dirty();
        p->setPos(x,y,dir);
        const auto result = aoi_.OnLocalEntityMoved(*p);
        if(result == AoiService::ApplyResult::kApplied || result == AoiService::ApplyResult::kIgnored)
            return Result::kOk;
        p->setPos(oldX,oldY,oldDir);
        if(!dirty) p->clearDirty();
        return result == AoiService::ApplyResult::kNeedSnapshot ? Result::kNotInScene : Result::kAoiRejected;
    }

    Result LeaveScene(uint64_t pid)
    {
        if(!players_.get(pid)) return Result::kNotFound;
        const auto id = aoi_.EntityOf(pid);
        if(!id) return Result::kOk;
        const auto result = aoi_.OnEntityLeave(*id);
        return result == AoiService::ApplyResult::kApplied || result == AoiService::ApplyResult::kIgnored
            ? Result::kOk : Result::kAoiRejected;
    }

private:
    PlayerManager& players_;
    AoiService& aoi_;
    const Map& map_;
};
#endif
