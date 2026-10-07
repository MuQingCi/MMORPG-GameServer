#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIGRID_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIGRID_H

#include "map.h"
#include "service/aoi/aoiTypes.h"

#include <cstdint>
#include <optional>
#include <vector>


struct AoiCell
{
    std::vector<EntityId> entys_;  //本格内的实例
    std::vector<EntityId> watchers_;
};

//AOI区域
class AoiGrid
{
public:                                           
    AoiGrid(uint32_t minX,
            uint32_t minY,
            uint32_t cols,
            uint32_t rows,
            uint32_t cellSize,
            uint32_t viewRadiusCells);

    // 坐标 -> 格子索引；越界返回 nullopt
    std::optional<uint32_t> TryCellOf(Position x, Position y) const;
    
     // 取格子，调用方保证索引合法（和 vector::operator[] 一致）
    const AoiCell& getCell(uint32_t cellIdx) const;

    // 以 (x, y) 为中心、半径 r_ 个格子覆盖的所有格子索引
    std::vector<uint32_t> getWatchCellIdx(Position x, Position y) const;

    // 两个格子是否互相可见（切比雪夫距离 <= r_）
    bool visible(uint32_t cellA, uint32_t cellB) const;

    void addEntity(EntityId entityId, uint32_t cell);
    void removeEntity(EntityId entityId, uint32_t cell);

    void addWatcher(EntityId observerId, uint32_t cell);
    void removeWatcher(EntityId observerId, uint32_t cell);

    const std::vector<EntityId>& entities(uint32_t cell) const;
    const std::vector<EntityId>& watchers(uint32_t cell) const;
    
    uint32_t viewRadiusCells() const { return r_; }

private:
    Position minX_ = 0;
    Position minY_ = 0;
    uint32_t cols_ = 0;
    uint32_t rows_ = 0;
    uint32_t cellSize_ = 0;
    uint32_t r_ = 0;

    std::vector<AoiCell> cells_;
};

#endif