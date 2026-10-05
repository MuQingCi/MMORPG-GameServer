#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_MAP_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_MAP_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using Position = uint32_t;

struct BlockEntity
{
    uint32_t pos_x  = 0;
    uint32_t pos_y  = 0;
    uint32_t length = 0;
    uint32_t width  = 0;
};

class Map
{
public:
    // 默认对象尚未加载。Load 失败不修改已有地图；不隐式依赖工作目录。
    Map() = default;
    ~Map() = default;
    static bool Load(const std::string& path, uint32_t cellSize, Map& out, std::string& err);

    //获取地图大小——返回(max_x,max_y)
    std::pair<uint32_t, uint32_t> getSize() const{ return {length_,width_}; }

    uint32_t getCols() const { return cellSize_ == 0 ? 0 : length_ / cellSize_ + (length_ % cellSize_ != 0); }
    uint32_t getRows() const { return cellSize_ == 0 ? 0 : width_ / cellSize_ + (width_ % cellSize_ != 0); }
    uint32_t getRaws() const { return getRows(); } // 兼容旧草稿接口
    uint32_t getCellSize() const { return cellSize_; }
    
    Position getMinX() const { return min_x; }
    Position getMinY() const { return min_y; }

    const std::vector<BlockEntity>& getBlock() const { return blocks_; }
    // 地图坐标采用 [0, length) × [0, width)，负值和上界均越界。
    bool contains(int32_t x, int32_t y) const;
    //判断目的地是否为障碍物
    bool DstIsBlock(int32_t x, int32_t y) const;
private:
    Position min_x = 0, min_y = 0;
    uint32_t length_ = 0;
    uint32_t width_ = 0;
    uint32_t cellSize_ = 0;
    std::vector<BlockEntity> blocks_;
};

#endif