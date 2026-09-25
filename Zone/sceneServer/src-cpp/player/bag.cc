#include "player/bag.h"

#include "log/logger.h"

#include <algorithm>
#include <utility>

Bag::Bag(size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity)
{
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

    if (items_.size() >= capacity_)
    {
        LOG_WARNING << "bag is full, capacity=" << capacity_ << " itemId=" << itemId;
        return false;
    }

    ItemStack st;
    st.itemId = itemId;
    st.count = num;
    st.slot = nextSlot_++;
    items_.emplace(itemId, st);
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
