#ifndef CLEARMOON_ACTOR_ACTORREF_H
#define CLEARMOON_ACTOR_ACTORREF_H

#include "common/hash.h"

#include <cstddef>
#include <cstdint>

/**
 * @brief Actor 引用：把"跨线程走消息、同线程直调"这条规则收进一个类型里
 *
 * 设计动机（见设计文档 3.2.4 Actor 边界）：
 *   - 跨 worker 必须走消息；同 worker 可以直接调用（同一 worker 本来就是串行的，
 *     直调安全且省一次消息往返）；
 *   - 一旦 Actor 的 worker 归属将来发生变化（重平衡、迁移、重连），
 *     "同 worker 直调优化"的判定会失效。因此业务代码不应自己比较 workerId，
 *     一律经由 ActorRef 判定，改归属时不用动业务代码。
 *
 * epoch 的用途：
 *   Actor 会被销毁重建（下线再上线、场景迁移）。异步回调（DB 结果、定时器）
 *   回来时，必须校验"发起时刻的 epoch"与"当前 epoch"是否一致，
 *   不一致说明是上一个生命周期的回调 —— 直接丢弃，绝不能写回到新 Actor 上。
 */
class ActorRef
{
public:
    using WorkerId = uint32_t;

    ActorRef() = default;
    ActorRef(uint64_t actorId, WorkerId workerId, uint64_t epoch = 0)
        : actorId_(actorId), workerId_(workerId), epoch_(epoch) {}

    // 玩家即 Actor：归属由纯函数决定，与进程内任何表无关
    static ActorRef ForPlayer(uint64_t playerId, size_t numWorkers, uint64_t epoch = 0)
    {
        return ActorRef(playerId, hashutil::WorkerForActor(playerId, numWorkers), epoch);
    }

    bool Valid() const { return actorId_ != 0; }
    uint64_t Id() const { return actorId_; }
    WorkerId Worker() const { return workerId_; }
    uint64_t Epoch() const { return epoch_; }

    // 是否落在本地线程：true = 可直调；false = 必须走 MsgBus
    bool IsLocal(WorkerId me) const { return actorId_ != 0 && workerId_ == me; }

    bool operator==(const ActorRef& o) const
    {
        return actorId_ == o.actorId_ && workerId_ == o.workerId_ && epoch_ == o.epoch_;
    }
    bool operator!=(const ActorRef& o) const { return !(*this == o); }

private:
    uint64_t actorId_ = 0;
    WorkerId workerId_ = 0;
    uint64_t epoch_ = 0;
};

#endif
