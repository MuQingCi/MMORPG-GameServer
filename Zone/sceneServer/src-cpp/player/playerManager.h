#ifndef CLEARMOON_SCENE_PLAYERMANAGER_H
#define CLEARMOON_SCENE_PLAYERMANAGER_H

#include "player/player.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

/**
 * @brief 玩家分片管理器（**每个逻辑线程一个实例**）
 *
 * 并发模型的定稿（对应建议书 E1，注释里那两个候选都不采用）：
 *   1. 不用"全局单例 + 专线逻辑线程独占"：threadPoolSize 已经是 8，
 *      把最热的玩家状态收敛到一个线程 = 用 8 线程的框架跑单线程性能；
 *   2. 不用"每玩家一把读写锁"：锁只保证单次访问原子，不保证
 *      "移动 -> 扣血 -> 死亡"三步不被别的线程插进来；且 1 万玩家 ≈ 560KB 锁开销、每次读两次原子操作；
 *   3. **采用 Actor 亲和分片**：workerId = hash(playerId) % N，
 *      分片内只被该 worker 访问，因此内部完全无锁；
 *      跨分片交互（A 打 B 在不同 worker）统一走消息，不跨分片直读。
 *
 * 附带收益：get() 返回裸指针是安全的（同线程内不存在并发删除）。
 * 玩家容量也随逻辑线程数水平扩展。
 */
class PlayerManager
{
public:
    explicit PlayerManager(uint32_t workerId);
    ~PlayerManager();

    PlayerManager(const PlayerManager&) = delete;
    PlayerManager& operator=(const PlayerManager&) = delete;

    /**
     * @brief 上线/换会话：保证返回一个有效 Player，并分配新的 epoch
     *
     * epoch 单调递增（分片内单线程自增，不需要原子操作）。
     * 重复上线（同 playerId 已存在）会：换绑 session、递增 epoch，
     * 使旧的异步回调（DB/定时器）自动失效。
     */
    Player* getOrCreate(uint64_t playerId, uint64_t session, uint32_t& epochOut);

    // 返回 nullptr 而不是引用：调用方必须判空（旧实现返回引用，find 失败即悬垂）
    Player* get(uint64_t playerId);
    const Player* get(uint64_t playerId) const;

    // 下线：返回是否真的移除了
    bool remove(uint64_t playerId);

    // 异步回调校验：Actor 是否还是发起时的那个生命周期
    bool validateEpoch(uint64_t playerId, uint32_t epoch) const;

    uint32_t epochOf(uint64_t playerId) const;
    uint64_t sessionOf(uint64_t playerId) const;

    // 跨线程可读的规模快照（阶段 1 任务 1.18）。
    // 为什么需要：stats()/playerCount() 由**主线程**调用，而 actors_ 只在 owner
    // 逻辑线程里被修改；直接读 actors_.size() 是"跨线程读容器"，属数据竞争（UB）。
    // 这里由 owner 线程在每次增删后写一份原子快照，读侧只 load —— 无锁、无竞争。
    size_t size() const { return sizeSnapshot_.load(std::memory_order_relaxed); }
    uint32_t workerId() const { return workerId_; }

    // 遍历（同线程安全）；fn 返回 false 可提前结束
    template<typename Fn>
    void forEach(Fn&& fn)
    {
        for (auto& kv : actors_)
        {
            if (!fn(kv.second))
                return;
        }
    }

private:
    uint32_t workerId_ = 0;
    uint32_t nextEpoch_ = 1;
    std::unordered_map<uint64_t, Player> actors_;

    // actors_ 的规模快照：只由 owner 线程写、任何线程可读（见 size() 的说明）
    std::atomic<size_t> sizeSnapshot_{0};
};

#endif
