#include "player/playerService.h"

PlayerService::PlayerService(PlayerManager& players, AoiService& aoi, const Map& map)
    : players_(players), aoi_(aoi), map_(map) 
{}

PlayerService::Result PlayerService::EnterScene(uint64_t pid, int32_t x, int32_t y, int32_t dir)
{
    auto* p = players_.get(pid);
    if(!p) 
        return Result::kNotFound;
    if(!map_.contains(x,y)) 
        return Result::kOutOfBounds;

    const auto id = aoi_.EntityOf(pid);
    if(id && aoi_.IsEntityActive(*id)) 
        return Result::kAlreadyInScene;

    const auto oldX = p->x(), oldY = p->y(), oldDir = p->dir();
    const bool changed = oldX != x || oldY != y || oldDir != dir;
    if(changed) 
        p->ensureRevisionAvailable();

    p->x_ = x; p->y_ = y; p->dir_ = dir;
    const auto result = aoi_.OnEntityEnter(*p);
    if(result != AoiService::ApplyResult::kApplied) 
    {
        p->x_ = oldX; p->y_ = oldY; p->dir_ = oldDir;
        return Result::kAoiRejected;
    }

    if(changed) 
        p->recordMutation();

    const auto& binding=p->clientBinding();
    if(GatewayPlayer::Valid(binding))
        aoi_.OnPlayerOnline(pid,binding.gatewayId,binding.clientEpoch);
    return Result::kOk;
}

PlayerService::Result PlayerService::Move(uint64_t pid, int32_t x, int32_t y, int32_t dir)
{
    auto* p = players_.get(pid);
    if(!p) 
        return Result::kNotFound;
    if(!map_.contains(x,y)) 
        return Result::kOutOfBounds;

    const auto oldX = p->x(), oldY = p->y(), oldDir = p->dir();
    const bool changed = oldX != x || oldY != y || oldDir != dir;
    if(changed) 
        p->ensureRevisionAvailable();

    p->x_ = x; p->y_ = y; p->dir_ = dir;
    const auto result = aoi_.OnLocalEntityMoved(*p);
    if(result == AoiService::ApplyResult::kApplied || result == AoiService::ApplyResult::kIgnored) 
    {
        if(changed) p->recordMutation();
        return Result::kOk;
    }
    p->x_ = oldX; p->y_ = oldY; p->dir_ = oldDir;
    return result == AoiService::ApplyResult::kNeedSnapshot ? Result::kNotInScene : Result::kAoiRejected;
}

PlayerService::Result PlayerService::LeaveScene(uint64_t pid)
{
    if(!players_.get(pid)) 
        return Result::kNotFound;
    
    const auto id = aoi_.EntityOf(pid);
    if(!id) 
        return Result::kOk;
    
    const auto result = aoi_.OnEntityLeave(*id);
    return result == AoiService::ApplyResult::kApplied || result == AoiService::ApplyResult::kIgnored
        ? Result::kOk : Result::kAoiRejected;
}