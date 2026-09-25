#ifndef CLEARMOON_PLAYER_H
#define CLEARMOON_PLAYER_H

#include "player/bag.h"

#include <cstdint>
#include <memory>
#include <string>

/**
 * @brief 玩家（Actor 的数据部分）
 *
 * 生命周期规则（与 Actor 模型配套）：
 *   - Player 只被"归属逻辑线程"访问，因此内部不加锁；
 *   - 跨线程要改它，只能给它所在的线程投消息；
 *   - 每次"上线/Actor 重建"都会分配一个新的 epoch，异步回调（DB/定时器）
 *     必须带 epoch 回来校验，避免迟到结果写进新生命周期的对象里。
 *
 * 移动语义：显式声明移动、删除拷贝。
 *   旧实现只声明了 ~Player()，这会抑制隐式移动构造，而成员里有 unique_ptr
 *   （不可拷贝），一旦放进 vector 或想 emplace 就会编译失败。
 */
class Player
{
public:
    Player() = default;
    ~Player() = default;

    Player(Player&&) = default;
    Player& operator=(Player&&) = default;
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // ---- 身份 ----
    uint64_t id() const { return playerId_; }
    void set_id(uint64_t v) { playerId_ = v; }

    uint32_t epoch() const { return epoch_; }
    void set_epoch(uint32_t v) { epoch_ = v; }

    uint64_t session() const { return session_; }
    void set_session(uint64_t v) { session_ = v; }

    // ---- 数值 ----
    uint16_t hp() const { return hp_; }
    void set_hp(uint16_t v) { hp_ = v; }
    void modifyHp(int32_t delta);

    uint16_t mp() const { return mp_; }
    void set_mp(uint16_t v) { mp_ = v; }
    void modifyMp(int32_t delta);

    uint32_t gold() const { return gold_; }
    void addGold(int64_t delta);

    // ---- 位置 ----
    int32_t x() const { return x_; }
    int32_t y() const { return y_; }
    int32_t dir() const { return dir_; }
    void setPos(int32_t x, int32_t y, int32_t dir);

    // ---- 背包 ----
    Bag& bag() { return bag_; }
    const Bag& bag() const { return bag_; }

    // ---- 脏标记与落盘节奏 ----
    // 存档策略：定期快照 + 关键操作立即落库（仅在登出时保存是数据事故的常见来源）
    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void clearDirty() { dirty_ = false; }

    int64_t lastActiveMs() const { return lastActiveMs_; }
    void touch(int64_t nowMs) { lastActiveMs_ = nowMs; }

private:
    uint64_t playerId_ = 0;
    uint32_t epoch_ = 0;
    uint64_t session_ = 0;

    uint16_t hp_ = 100;
    uint16_t mp_ = 50;
    uint32_t gold_ = 0;
    int32_t x_ = 0;
    int32_t y_ = 0;
    int32_t dir_ = 0;

    Bag bag_;

    bool dirty_ = false;
    int64_t lastActiveMs_ = 0;
};

#endif
