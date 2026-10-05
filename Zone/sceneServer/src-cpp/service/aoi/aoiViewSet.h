#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIVIEWSET_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIVIEWSET_H

#include "service/aoi/aoiGrid.h"
#include "aoiTypes.h"
#include <atomic>
#include <set>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

struct ObserverSnapshot
{
    //实例id,上次入队版本号
    std::unordered_map<uint64_t, uint64_t> committed_;
    //
    bool Resync = false;
};


/**
 * @brief 当前类只保存实例ID对应的已提交的AOI快照——即客户端已能看到的实例ID
 * 
 */

class AoiViewSet
{
public:
    AoiViewSet();
    ~AoiViewSet();

    ObserverSnapshot& getViewByEntityId(EntityId eid);

    void processPending();
    
private:
    //实例ID->对应快照
    std::unordered_map<EntityId, ObserverSnapshot> views_;

    std::map<EntityId, uint64_t> pendings_;     //待补发AOI事件->版本号
    std::atomic<uint64_t> failedSendNum_ = {0};//发送失败计数器
};

#endif