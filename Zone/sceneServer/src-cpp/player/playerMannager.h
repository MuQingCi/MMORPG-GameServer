#ifndef CLEARMOON_SCENE_PLAYERMANNAGER_H
#define CLEARMOON_SCENE_PLAYERMANNAGER_H

#include "player/player.h"
#include <cstdint>
#include <unordered_map>

/**
 * @brief 全场景玩家管理类,内部提供添加、踢出玩家接口，提供Player获取接口
 目前两种设计如下:
 1.由sceneServer持有(全局单例),当前想抽出一个逻辑线程来单独访问并修改这个类；线程安全做法:网络层接收玩家上线、玩家事件都将其封装为Msg并将其分发到这个逻辑线程中由这个逻辑线程单独修改玩家状态等;
 2.每个玩家都持有一把读写锁，多个逻辑线程都可以访问这个玩家管理类的所有玩家，支持同一个玩家状态时支持多读，但有修改操作时阻塞；
 * 
 */
class PlayerManager
{
public:
    PlayerManager();
    ~PlayerManager();

    bool addPlayer(uint64_t playerId);
    bool tickPlayer(uint64_t playerId);

    const Player& getPlayer(uint64_t);

private:
    std::unordered_map<uint64_t, Player> players_;
};

#endif