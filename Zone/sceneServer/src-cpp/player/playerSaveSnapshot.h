#ifndef SCENE_PLAYER_SAVE_SNAPSHOT_H
#define SCENE_PLAYER_SAVE_SNAPSHOT_H

#include "player/item.h"
#include <cstdint>
#include <vector>

// 仅保存固定值，不包含 Actor 指针、网络绑定或 AOI 状态。
// saveVersion 和 saveLifecycleToken 由保存协调器分配。
struct PlayerSaveSnapshot {
    uint64_t playerId = 0;
    uint16_t hp = 100;
    uint16_t mp = 50;
    uint32_t gold = 0;
    int32_t x = 0, y = 0, dir = 0;
    uint32_t sceneId = 1;
    uint32_t bagCapacity = 64;
    uint32_t bagNextSlot = 0; // 65536 表示无法再分配新槽位。
    std::vector<ItemStack> items;
    uint64_t saveVersion = 0;
    uint64_t capturedRevision = 0;
    uint64_t saveLifecycleToken = 0;
};

#endif