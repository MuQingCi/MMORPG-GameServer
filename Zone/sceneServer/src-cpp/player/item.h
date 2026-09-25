#ifndef CLEARMOON_SCENE_ITEM_H
#define CLEARMOON_SCENE_ITEM_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

/**
 * @brief 物品的**静态配置**（id/类型/名字）
 *
 * 注意区分两件事：
 *   - Item    : 配置表里的一行（只读、可全局共享）
 *   - ItemStack: 玩家背包里的一格（数量、槽位，可写、属于某个玩家 Actor）
 * 旧代码把 Item 直接当 map 的 key 且只比较 id_，type_/name_ 成了挂在 key 上的
 * 冗余负载 —— 这正是"语义混乱"的来源。这里显式拆开。
 */
struct Item
{
    uint32_t    id = 0;
    uint16_t    type = 0;
    std::string name;

    bool operator==(const Item& o) const noexcept { return id == o.id; }
};

struct ItemHash
{
    size_t operator()(const Item& it) const noexcept
    {
        return std::hash<uint32_t>{}(it.id);
    }
};

// 背包里的一格：itemId -> 数量 + 槽位
struct ItemStack
{
    uint32_t itemId = 0;
    uint16_t count = 0;
    uint16_t slot = 0;
};

#endif
