#ifndef SCENE_PLAYER_PERSISTENCE_H
#define SCENE_PLAYER_PERSISTENCE_H

#include "db/playerSnapshotDao.h"
#include "db/taskDelivery.h"
#include "player/player.h"
#include <optional>

// 仅在所属线程使用。消息传递层负责投递重试；本类绝不
// 因超时重新执行已接受任务；未解决的失败持续保留固定数据。
class PlayerPersistence 
{
public:
    static constexpr size_t kMaxPlayers = 65536;
    using Submit = std::function<bool(Msg)>;
    enum class State { Idle, AwaitResult, Blocked };

    struct Record {
        uint32_t actorEpoch = 0;
        uint64_t token = 0, nextVersion = 0, confirmedVersion = 0, confirmedRevision = 0;
        uint64_t taskId = 0;
        State state = State::Idle;
        bool offline = false;
        std::optional<PlayerSaveSnapshot> active, queued;
        std::string error;
    };

    // 在 Actor 销毁前冻结快照。queued 保存可替换的最新完整
    // 快照，而非增量；离线记录未解决前不允许回收。
    bool request(Player& player, bool offline, const Submit& submit);

    void tick(const Submit& submit);
    bool owns(uint64_t taskId) const;

    // 在 Actor epoch 校验前调用。只有明确成功才确认保存；
    // 被新版本取代、冲突或未知结果都不能当作保存成功确认。
    bool complete(uint64_t taskId, const dbv::DbResult& result, Player* live);
    bool blocksLogin(uint64_t playerId) const;
    const Record* find(uint64_t playerId) const;
    bool unsettled() const;
private:
    static void dispatch(Record& r, const Submit& submit) {
        if(r.state != State::Idle || !r.queued) 
            return;
        dbv::DbTask task; std::string error;
        if(!PlayerSnapshotDao::buildSave(*r.queued,task,error)) 
        {
            r.state = State::Blocked; r.error = error; return;
        }
        if(!r.taskId) 
            r.taskId = PendingDbTasks::newTaskId();

        Msg message;
        message.diagnosticSaveTask = true;
        message.head.msgType = MSGTYPE_DB_TASK_MYSQL;
        message.head.Module = Module::SYS;
        message.head.Method = Method::SYS_FLUSH;
        message.head.playerId = task.playerId;
        message.head.epoch = r.actorEpoch;
        message.head.taskId = r.taskId;
        message.body = task.Encode();
        if(!submit(std::move(message))) 
            return;
        r.active = std::move(r.queued); r.queued.reset(); r.state = State::AwaitResult;
    }
    uint64_t nextToken_ = 0;

    // 玩家 ID → 保存记录。
    std::unordered_map<uint64_t,Record> records_;
};
#endif