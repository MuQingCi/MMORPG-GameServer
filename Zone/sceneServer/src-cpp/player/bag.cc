#include "player/bag.h"

#include "log/logger.h"

#include <algorithm>
#include <utility>
#include <stdexcept>
#include <unordered_set>

Bag::Bag(size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity)
{
    if (capacity_ > 65536) 
        throw std::invalid_argument("bag capacity exceeds slot range");
}

Bag::~Bag() = default;

std::vector<ItemStack> Bag::view() const
{
    std::vector<ItemStack> out;
    out.reserve(items_.size());
    for (const auto& kv : items_)
        out.push_back(kv.second);

    // 按槽位稳定输出，方便对账/回包比对
    std::sort(out.begin(), out.end(),
              [](const ItemStack& a, const ItemStack& b) { return a.slot < b.slot; });
    return out;
}

bool Bag::addItem(uint32_t itemId, uint16_t num)
{
    if (itemId == 0 || num == 0)
        return false;

    auto it = items_.find(itemId);
    if (it != items_.end())
    {
        // 数量上溢保护：宁可拒绝也不要静默回绕
        if (static_cast<uint32_t>(it->second.count) + num > 0xFFFFu)
            return false;
        it->second.count = static_cast<uint16_t>(it->second.count + num);
        return true;
    }

    if (items_.size() >= capacity_ || nextSlot_ >= 65536)
    {
        LOG_WARNING << "bag is full, capacity=" << capacity_ << " itemId=" << itemId;
        return false;
    }

    ItemStack st;
    st.itemId = itemId;
    st.count = num;
    st.slot = static_cast<uint16_t>(nextSlot_);
    items_.emplace(itemId, st);
    ++nextSlot_;
    return true;
}

bool Bag::reduceItem(uint32_t itemId, uint16_t num)
{
    if (itemId == 0 || num == 0)
        return false;

    auto it = items_.find(itemId);
    if (it == items_.end() || it->second.count < num)
        return false;

    it->second.count = static_cast<uint16_t>(it->second.count - num);
    if (it->second.count == 0)
        items_.erase(it);
    return true;
}

uint16_t Bag::countOf(uint32_t itemId) const
{
    auto it = items_.find(itemId);
    return it == items_.end() ? 0 : it->second.count;
}

void Bag::clear()
{
    items_.clear();
    nextSlot_ = 0;
}

bool Bag::restore(size_t capacity, uint32_t nextSlot, const std::vector<ItemStack>& items)
{
    if (capacity == 0 || capacity > 65536 || nextSlot > 65536 || items.size() > capacity)
        return false;
    std::unordered_map<uint32_t, ItemStack> restored;
    std::unordered_set<uint16_t> slots;
    for (const auto& item : items) {
        if (!item.itemId || !item.count || item.slot >= nextSlot ||
            !slots.insert(item.slot).second || !restored.emplace(item.itemId, item).second)
            return false;
    }
    items_.swap(restored);
    capacity_ = capacity;
    nextSlot_ = nextSlot;
    return true;
}
