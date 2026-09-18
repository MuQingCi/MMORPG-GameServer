#ifndef CLEARMOON_PLAYER_H
#define CLEARMOON_PLAYER_H

#include "msg.h"
#include "player/bag.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <map>
/**
 * @brief 可以为其创建内存池 + 对象池，使得Player类可以在内存池/对象池中创建、销毁以减小本类构造、析构开销
 * 内部数据由playerId查询数据库后载入
 */

class Player
{
public:
    Player();
    ~Player();

    

    void move(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy);
    void attack(uint64_t targetId);
    std::map<std::string&, size_t> viewBag();
    
private:
    //玩家Id、HP、MP等玩家数据
    uint64_t playerId_;
    uint16_t hp_;
    uint16_t mp_;
    uint32_t gold_;
    size_t   attackDistance_;
    std::unique_ptr<Bag> bagPtr_;

    int64_t lastActiveTime_;
};

#endif