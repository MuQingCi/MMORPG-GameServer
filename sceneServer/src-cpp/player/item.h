#ifndef CLEARMOON_SCENE_ITEM_H
#define CLEARMOON_SCENE_ITEM_H

#include <cstdint>
#include <string>


struct Item
{
    uint32_t    id_;
    uint16_t    type_;
    std::string name_;

    bool operator==(const Item& o) const noexcept 
    {
        return id_ == o.id_;
    }
};


struct ItemHash {
    size_t operator()(const Item& it) const noexcept 
    {
        return std::hash<uint32_t>{}(it.id_);
    }
};
#endif