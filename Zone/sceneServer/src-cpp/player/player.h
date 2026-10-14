#ifndef CLEARMOON_PLAYER_H
#define CLEARMOON_PLAYER_H

#include "player/bag.h"
#include "player/playerSaveSnapshot.h"
#include "base/gatewayPlayer.h"

#include <cstdint>
#include <memory>
#include <optional>
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
    const ClientData& clientBinding() const { return clientBinding_; }
    void set_clientBinding(const ClientData& v) { clientBinding_ = v; }

    // ---- 数值 ----
    uint16_t hp() const { return hp_; }
    void set_hp(uint16_t v);
    void modifyHp(int32_t delta);

    uint16_t mp() const { return mp_; }
    void set_mp(uint16_t v);
    void modifyMp(int32_t delta);

    uint32_t gold() const { return gold_; }
    void addGold(int64_t delta);

    // ---- 位置 ----
    int32_t x() const { return x_; }
    int32_t y() const { return y_; }
    int32_t dir() const { return dir_; }
    void setPos(int32_t x, int32_t y, int32_t dir);

    // ---- 背包 ----
    const Bag& bag() const { return bag_; }
    bool addItem(uint32_t itemId, uint16_t count);
    bool consumeItem(uint32_t itemId, uint16_t count);
    void clearBag();

    // 由所属线程可信业务提供计划，不能使用客户端自报价格。
    struct EconomyRequest {
        std::string operationId;
        uint32_t cost = 0, itemId = 0;
        uint16_t count = 0;
        uint32_t rulesVersion = 0;
        bool operator==(const EconomyRequest&) const = default;
    };
    bool reserveEconomy(const EconomyRequest& request);
    bool economyPending() const { return economyReservation_.has_value(); }
    // 预留时冻结候选状态。持久化层必须将本计划与
    // 前序写入串行执行，不能将购买前的实时经济状态作为后续保存内容。
    const PlayerSaveSnapshot* economyCandidate() const {
        return economyReservation_ ? &economyReservation_->candidate : nullptr;
    }
    // 只有保存协调器取得持久成功证据后才能调用提交。
    // 只有确认拒绝或回滚后才能取消预留，超时不能作为取消依据。
    bool commitEconomy(const std::string& operationId);
    bool cancelEconomy(const std::string& operationId);

    uint64_t mutationRevision() const { return mutationRevision_; }
    uint64_t persistedVersion() const { return persistedVersion_; }
    uint64_t persistedRevision() const { return persistedRevision_; }

    void confirmSave(uint64_t version, uint64_t revision) {
        if (revision > mutationRevision_ || version < persistedVersion_) return;
        persistedVersion_ = version;
        persistedRevision_ = revision;
        if (revision == mutationRevision_) clearDirty();
    }
    uint32_t sceneId() const { return sceneId_; }
    void setSceneId(uint32_t sceneId);
    PlayerSaveSnapshot freezeSaveSnapshot(uint64_t saveVersion, uint64_t lifecycleToken) const;
    // 仅供加载使用，不能用于已活动的 Actor 或 AOI 实体。
    // 保留 session 和 Actor epoch；无效输入不改变任何状态。
    bool restoreSaveSnapshot(const PlayerSaveSnapshot& snapshot);

    // ---- 脏标记与落盘节奏 ----
    // 存档策略：定期快照 + 关键操作立即落库（仅在登出时保存是数据事故的常见来源）
    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; } // 仅用于保存调度，不表示数据发生新变化。
    void clearDirty() { dirty_ = false; }

    int64_t lastActiveMs() const { return lastActiveMs_; }
    void touch(int64_t nowMs) { lastActiveMs_ = nowMs; }

private:
    friend class PlayerService; // 暂存位置、通知 AOI，然后记录一次数据变化。
    void ensureRevisionAvailable() const;
    void recordMutation();

    //玩家标识
    uint64_t playerId_ = 0;
    uint32_t epoch_ = 0;
    uint64_t session_ = 0;
    ClientData clientBinding_;
    //数据
    uint16_t hp_ = 100;
    uint16_t mp_ = 50;
    uint32_t gold_ = 0;
    int32_t x_ = 0;
    int32_t y_ = 0;
    int32_t dir_ = 0;
    uint32_t sceneId_ = 1;
    uint64_t mutationRevision_ = 0;
    uint64_t persistedVersion_ = 0;
    uint64_t persistedRevision_ = 0;

    Bag bag_;

    struct EconomyReservation {
        EconomyRequest request;
        PlayerSaveSnapshot candidate;
        Bag preparedBag;
    };
    std::optional<EconomyReservation> economyReservation_;

    bool dirty_ = false;
    int64_t lastActiveMs_ = 0;
};

#endif
