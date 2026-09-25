#include "player/player.h"

namespace
{
// 全部数值都有上下界钳制：溢出静默回绕会造成"打不死/钱变负数"这类线上事故
constexpr uint16_t kMaxHp = 65535;
constexpr uint16_t kMaxMp = 65535;
constexpr uint32_t kMaxGold = 0xFFFFFFFFu;
}  // namespace

void Player::modifyHp(int32_t delta)
{
    int64_t v = static_cast<int64_t>(hp_) + delta;
    if (v < 0)
        v = 0;
    if (v > kMaxHp)
        v = kMaxHp;
    hp_ = static_cast<uint16_t>(v);
    markDirty();
}

void Player::modifyMp(int32_t delta)
{
    int64_t v = static_cast<int64_t>(mp_) + delta;
    if (v < 0)
        v = 0;
    if (v > kMaxMp)
        v = kMaxMp;
    mp_ = static_cast<uint16_t>(v);
    markDirty();
}

void Player::addGold(int64_t delta)
{
    int64_t v = static_cast<int64_t>(gold_) + delta;
    if (v < 0)
        v = 0;
    if (v > kMaxGold)
        v = kMaxGold;
    gold_ = static_cast<uint32_t>(v);
    markDirty();
}

void Player::setPos(int32_t x, int32_t y, int32_t dir)
{
    x_ = x;
    y_ = y;
    dir_ = dir;
    markDirty();
}
