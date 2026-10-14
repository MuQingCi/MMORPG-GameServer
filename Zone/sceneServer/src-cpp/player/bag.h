#ifndef CLEARMOON_SCENE_BAG_H
#define CLEARMOON_SCENE_BAG_H

#include "player/item.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

/**
 * @brief 玩家背包（属于某个玩家 Actor，只被其归属逻辑线程访问，因此内部无锁）
 *
 * 数据结构：itemId -> {count, slot}
 *   - 用 itemId 做 key、数量 uint16、并显式记录槽位，语义与容量都明确。
 */
class Bag
{
public:
    explicit Bag(size_t capacity = 64);
    ~Bag();

    Bag(Bag&&) = default;
    Bag& operator=(Bag&&) = default;
    Bag(const Bag&) = delete;
    Bag& operator=(const Bag&) = delete;

    // 只读视图（返回副本，避免把内部容器暴露给调用方后又被并发改）
    std::vector<ItemStack> view() const;

    bool addItem(uint32_t itemId, uint16_t num);
    bool reduceItem(uint32_t itemId, uint16_t num);
    uint16_t countOf(uint32_t itemId) const;
    void clear();

    size_t capacity() const { return capacity_; }
    size_t used() const { return items_.size(); }
    uint32_t nextSlot() const { return nextSlot_; }
    // 加载固定持久状态，先校验再替换当前背包。
    bool restore(size_t capacity, uint32_t nextSlot, const std::vector<ItemStack>& items);

private:
    size_t capacity_ = 64;
    uint32_t nextSlot_ = 0;
    std::unordered_map<uint32_t, ItemStack> items_;
};

#endif
