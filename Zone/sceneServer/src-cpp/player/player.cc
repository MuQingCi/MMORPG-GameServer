#include "player/player.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

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
    set_hp(static_cast<uint16_t>(v));
}

void Player::modifyMp(int32_t delta)
{
    int64_t v = static_cast<int64_t>(mp_) + delta;
    if (v < 0)
        v = 0;
    if (v > kMaxMp)
        v = kMaxMp;
    set_mp(static_cast<uint16_t>(v));
}

void Player::addGold(int64_t delta)
{
    if(economyPending()) 
        throw std::logic_error("economic operation pending");
    // 先限制增减范围再相加，避免 Lua 极端参数导致有符号溢出。
    const auto clamped = std::clamp<int64_t>(delta, -static_cast<int64_t>(gold_),
                                           static_cast<int64_t>(kMaxGold) - gold_);
    const auto value = static_cast<uint32_t>(static_cast<int64_t>(gold_) + clamped);
    if (value == gold_) 
        return;
    ensureRevisionAvailable();
    gold_ = value;
    recordMutation();
}

void Player::setPos(int32_t x, int32_t y, int32_t dir)
{
    if (x_ == x && y_ == y && dir_ == dir) 
        return;
    ensureRevisionAvailable();
    x_ = x;
    y_ = y;
    dir_ = dir;
    recordMutation();
}

void Player::ensureRevisionAvailable() const
{
    if (mutationRevision_ == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("player mutation revision exhausted");
}

void Player::recordMutation()
{
    ensureRevisionAvailable();
    ++mutationRevision_;
    markDirty();
}

void Player::set_hp(uint16_t value)
{
    if (hp_ == value) 
        return;
    ensureRevisionAvailable();
    hp_ = value;
    recordMutation();
}

void Player::set_mp(uint16_t value)
{
    if (mp_ == value) return;
    ensureRevisionAvailable();
    mp_ = value;
    recordMutation();
}

void Player::setSceneId(uint32_t sceneId)
{
    if (!sceneId) 
        throw std::invalid_argument("scene id must be positive");
    if (sceneId_ == sceneId) 
        return;
    ensureRevisionAvailable();
    sceneId_ = sceneId;
    recordMutation();
}

bool Player::addItem(uint32_t itemId, uint16_t count)
{
    if(economyPending()) 
        return false;
    ensureRevisionAvailable();
    if (!bag_.addItem(itemId, count)) 
        return false;
    recordMutation();
    return true;
}

bool Player::consumeItem(uint32_t itemId, uint16_t count)
{
    if(economyPending()) 
        return false;
    ensureRevisionAvailable();
    if (!bag_.reduceItem(itemId, count)) 
        return false;
    recordMutation();
    return true;
}

void Player::clearBag()
{
    if(economyPending()) 
        throw std::logic_error("economic operation pending");
    if (!bag_.used() && !bag_.nextSlot()) 
        return;
    ensureRevisionAvailable();
    bag_.clear();
    recordMutation();
}

PlayerSaveSnapshot Player::freezeSaveSnapshot(uint64_t saveVersion, uint64_t lifecycleToken) const
{
    if (saveVersion > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        throw std::invalid_argument("save version exceeds database range");

    //保存当前玩家状态快照
    PlayerSaveSnapshot snapshot;
    snapshot.playerId = playerId_;
    snapshot.hp = hp_; snapshot.mp = mp_; snapshot.gold = gold_;
    snapshot.x = x_; snapshot.y = y_; snapshot.dir = dir_;
    snapshot.sceneId = sceneId_;
    snapshot.bagCapacity = static_cast<uint32_t>(bag_.capacity());
    snapshot.bagNextSlot = bag_.nextSlot();
    snapshot.items = bag_.view();
    snapshot.saveVersion = saveVersion;
    snapshot.capturedRevision = mutationRevision_;
    snapshot.saveLifecycleToken = lifecycleToken;
    return snapshot;
}

bool Player::restoreSaveSnapshot(const PlayerSaveSnapshot& snapshot)
{
    if(economyPending()) 
        return false;

    const auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (!snapshot.playerId || snapshot.playerId > max || 
        !snapshot.sceneId ||snapshot.saveVersion > max || 
        (playerId_ && playerId_ != snapshot.playerId))
        return false;
    
    Bag restored;
    if (!restored.restore(snapshot.bagCapacity, snapshot.bagNextSlot, snapshot.items))
        return false;

    bag_ = std::move(restored);
    playerId_ = snapshot.playerId;
    hp_ = snapshot.hp; mp_ = snapshot.mp; gold_ = snapshot.gold;
    x_ = snapshot.x; y_ = snapshot.y; dir_ = snapshot.dir;
    sceneId_ = snapshot.sceneId;
    mutationRevision_ = 0; // 新加载的内存生命周期，不沿用快照中的旧修订号。
    persistedVersion_ = snapshot.saveVersion;
    persistedRevision_ = 0;
    dirty_ = false;
    return true;
}

bool Player::reserveEconomy(const EconomyRequest& request)
{
    if(economyReservation_) 
        return economyReservation_->request==request;

    if(!playerId_ || request.operationId.empty() || 
        request.operationId.size()>128 ||!request.itemId || 
        !request.count || !request.rulesVersion || request.cost>gold_)
        return false;

    ensureRevisionAvailable();
    EconomyReservation reservation;
    reservation.request=request;
    reservation.candidate=freezeSaveSnapshot(persistedVersion_,0);
    if(!reservation.preparedBag.restore(bag_.capacity(),bag_.nextSlot(),bag_.view()) ||
       !reservation.preparedBag.addItem(request.itemId,request.count)) return false;
    reservation.candidate.gold=gold_-request.cost;
    reservation.candidate.items=reservation.preparedBag.view();
    reservation.candidate.bagNextSlot=reservation.preparedBag.nextSlot();
    economyReservation_=std::move(reservation);
    return true;
}

bool Player::commitEconomy(const std::string& operationId)
{
    if(!economyReservation_ || economyReservation_->request.operationId!=operationId) 
        return false;
    ensureRevisionAvailable();
    // 安装结果后不再分配内存，也不加载无关状态。
    gold_=economyReservation_->candidate.gold;
    bag_=std::move(economyReservation_->preparedBag);
    economyReservation_.reset();
    recordMutation();
    return true;
}

bool Player::cancelEconomy(const std::string& operationId)
{
    if(!economyReservation_ || economyReservation_->request.operationId!=operationId) 
        return false;
    economyReservation_.reset();
    return true;
}
