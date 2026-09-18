#ifndef CLEARMOON_SCENE_BAG_H
#define CLEARMOON_SCENE_BAG_H

#include "player/item.h"
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <set>

/**
 * @brief 玩家背包，对外开放查询、添加、消费Item接口,附带将所有物品全部存入仓库的接口
 * 
 */
class Bag
{
public:
    Bag(size_t bagSize = 64);
    ~Bag();

    //浏览背包物品
    std::set<std::string&, size_t> viewBag();
    bool addItem(uint32_t itemId, uint16_t num);
    bool reduceItem(uint32_t itemId, uint16_t num);
    //将所有物品存入仓库
    bool clear();
private:
    std::unordered_map<Item, uint8_t, ItemHash> itemLists_;
};

#endif