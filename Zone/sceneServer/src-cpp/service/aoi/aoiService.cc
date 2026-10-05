#include "aoiService.h"
#include "service/aoi/aoiReplica.h"
#include "service/aoi/map.h"
#include <memory>

AoiService::AoiService(workerId selfWorkerId, 
                       const AoiConfig& cfg, 
                       AoiPushSink* sink,
                       const Map& map) 
                    :  wId_(selfWorkerId),
                       aoiConfig_(cfg),
                       viewSet_(std::make_unique<AoiViewSet>()),
                       replica_(std::make_unique<AoiReplica>(selfWorkerId,cfg, map))
{

}

// ---- 生命周期----
void AoiService::OnEntityEnter(Player& p)
{

}   

void AoiService::OnEntityReenter(Player& p)
{

}   

void AoiService::OnEntityLeave(EntityId id)
{

}   

void AoiService::OnLocalEntityMoved(Player& p)
{

}   


// ---- 内部事件----
void AoiService::ApplyImmediately(const AoiEvent& ev)
{

}   

void AoiService::CoalesceBatch(const AoiBatchEvent& ev)
{

} 

void AoiService::ApplyOwnerSnapshot(const AoiOwnerSnapshot& ev)
{

} 

// ---- 会话生命周期（§7.6.1、D4.5；来自网关，非网络业务帧）----
void AoiService::OnPlayerOnline(uint64_t pid, uint64_t gatewayId, uint32_t clientViewEpoch)
{

}

void AoiService::OnPlayerRebind(uint64_t pid, uint32_t oldEpoch, uint32_t newEpoch)
{

}

void AoiService::OnPlayerOffline(uint64_t pid, uint32_t clientViewEpoch, uint8_t reason)
{

}

void AoiService::OnOnlineSetSnapshot(const std::vector<SessionEntry>& online)
{

}   


// ---- 客户端重同步请求（§7.6.3；C2S，经 §11.2 的 C++ 分支进入）----
void AoiService::OnViewResyncReq(uint64_t observerBusinessId, const std::string& body)
{

}


// ---- tick 与丢帧反馈 ----
void AoiService::OnTick(int64_t now)
{

}

// §8.1 顺序契约 + 四道预算闸
AoiViewSet& AoiService::views()
{

}

// ---- 只读查询与统计 ----
bool AoiService::ViewOf(EntityId observer, std::vector<AoiEntity>& out) const
{
}  // scene.view
AoiStats AoiService::Snapshot() const
{

}   
                                       // stats()（原子快照）