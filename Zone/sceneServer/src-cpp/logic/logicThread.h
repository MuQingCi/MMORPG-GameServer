#ifndef CLEARMOON_SCENESERVER_LOGIC_LOGICTHREAD_H
#define CLEARMOON_SCENESERVER_LOGIC_LOGICTHREAD_H

#include "common/msg.h"
#include "common/msgBus.h"
#include "db/redisKey.h"
#include "db/taskDelivery.h"
#include "player/playerPersistence.h"
#include "lua/luaApiCtx.h"
#include "service/aoi/aoiService.h"
#include "logic/workerPushSink.h"
#include "metrics/sceneMetrics.h"
#include "player/playerService.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

class Player;
class PlayerManager;
class LuaEnv;
class ITimerService;

namespace RouteTable
{
struct Route;
}

/**
 * @brief 逻辑线程（Actor 的执行体）：每个线程独占一个 lua_State 与一份玩家分片
 *
 * 线程内所有权的三条规则：
 *   1. **玩家分片属于本线程**：PlayerManager 是成员而不是全局单例，
 *      因为归属是 hash(playerId) % N 的纯函数 —— 分片内不存在并发访问，因此不需要锁；
 *   2. **lua_State 属于本线程**：跨线程调用 Lua 会破坏栈与 GC；
 *   3. **DB 结果必须回到本线程**：DB 线程只把 Msg 投回 srcWorkerId，
 *      由本线程进 Lua（DispatchDbAck），绝不从 DB 线程直接碰 state。
 *
 * 消息处理的三类入口：
 *   连接类：CONN_NEW / CONN_CLOSE / CONN_DATA（网络线程投来）
 *   异步回调：DB_RESULT_MYSQL / DB_RESULT_REDIS / TIMER_FIRE
 *   控制类：RELOAD / SHUTDOWN
 *
 * 异步回调的共同点：**必须先校验 epoch**。发起时 Actor 可能已下线重建，
 * 迟到结果要么写错对象，要么直接丢弃。
 */

class LogicThread
{
public:
    struct Config
    {
        std::string luaDir = "./lua_script";
        int64_t idleWaitMs = 5;              // 队列空时的阻塞等待上限
        int64_t flushIntervalMs = 30000;     // 定期快照间隔（防宕机丢数据）
        int64_t scanIdleMs = 10000;          // 空闲连接扫描间隔
        uint64_t luaInstructionLimit = 10000000;  // 0 = 关闭 Lua 指令预算
        // Redis 命名空间：脚本发的每条 Redis 命令都按它校验 key 前缀（见 db/redisKey.h）
        RedisNamespace redisNs;
        bool persistenceEnabled = true; // 无 DB 的开发模式中显式关闭持久化。
        int64_t shutdownDrainMs = 5000;
    };

    LogicThread(uint32_t threadId, MsgBus& bus, const Config& cfg, const AoiConfig& aoiCfg,
                const Map& map, WorkerMetrics& metrics);

    ~LogicThread();

    LogicThread(const LogicThread&) = delete;
    LogicThread& operator=(const LogicThread&) = delete;

    bool start();
    void stop();
    void requestDrain();
    bool drainFinished() const { return drainFinished_.load(); }
    bool drainSucceeded() const { return drainSucceeded_.load(); }

    // 由 SceneServer 在 start 前注入（LuaEnv 未绑定时 timer API 会明确报错而不是野指针崩溃）
    void bindTimer(ITimerService* timer);

    uint32_t threadId() const { return threadId_; }
    PlayerManager& players();   // 定义在 .cc：头文件里 PlayerManager 还是不完整类型
    size_t playerCount() const; // 同样定义在 .cc

private:
    struct WorkerMetricsRecorder
    {
        uint64_t handled = 0, dropped = 0, handlerUs = 0, activeUs = 0;
        uint64_t handlerMaxUs = 0, cycleMaxUs = 0, lifetimeCycleMaxUs = 0;
        void publish(WorkerMetrics& target) noexcept;
    } recorder_;
    void run();
    void handleMsg(Msg& m);

    void onConnNew(Msg& m);
    void onConnClose(Msg& m);
    void onNetMsg(Msg& m);
    void onPlayerBinding(Msg& m);
    bool logoutPlayer(uint64_t playerId);
    void onDbResult(Msg& m);
    void onTimer(Msg& m);
    void onReload(Msg& m);
    void onService(Msg& m);
    void advanceAoiRecovery(int64_t now);

    void onIdle();             // 空闲内务：仅执行空闲扫描
    void doReloadIfNeeded();   // 主循环安全点热更
    void drainPending();

    // 懒创建 Actor：第一条带 playerId 的业务消息就是"玩家进入本场景"
    Player* ensureActor(uint64_t playerId, uint64_t session, uint32_t& epochOut);

    void sendRetTip(const Msg& request, int32_t code, const char* text);
    void sendToSession(uint64_t session, const RouteTable::Route* route, uint64_t seq,
                       uint64_t playerId, const std::string& body);
    void flushDirtyPlayers();
    void advancePlayerLoads();
    bool handlePlayerLoadResult(const Msg& message, const dbv::DbResult& result);

    std::thread thread_;
    uint32_t threadId_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopRequested_{false};

    Config cfg_;
    MsgBus* m_bus_ = nullptr;
    WorkerMetrics* metrics_ = nullptr;

    std::unique_ptr<LuaEnv> lua_;
    // std::unique_ptr<PlayerManager> players_;
    PlayerManager players_;
    ITimerService* timer_ = nullptr;
    
    WorkerPushSink workerPushSink_;
    AoiService aoiService_;
    PlayerService playerService_;

    LuaApiCtx apiCtx_;

    // 离线后保留绑定水位，拒绝延迟上线通知重新创建旧会话。
    std::unordered_map<uint64_t, ClientData> latestClientBindings_;

    // 完整 owner 快照回复：每目标最多保留一个待重投包。
    std::unordered_map<workerId, AoiWorkerPublishRequest> pendingAoiSnapshots_;
    //owner worker → 下次允许请求快照的时间
    std::unordered_map<workerId, int64_t> nextSnapshotRequestAt_;
    //整个恢复调度下次允许运行的时间
    int64_t nextAoiRecoveryAt_ = 0;

    // 热更：目标版本 + 目标目录（由广播消息带来，每个 worker 独立应用）
    std::atomic<uint64_t> pendingLuaVersion_{0};
    std::string pendingLuaDir_;
    uint64_t myLuaVersion_ = 0;

    int64_t lastFlushMs_ = 0;
    int64_t lastScanMs_ = 0;

    //可靠任务投递——待处理队列
    //根据state判断任务处于什么状态，用一个容器存储以减少同步数和产生空隙时间

    //存储跨线程 DB 请求投递、等待结果和 ACK 状态
    PendingDbTasks pendingTasks_;

    //存储已冻结的周期或离线保存状态
    PlayerPersistence persistence_;

    struct LoadState {
        //触发本次加载的可信网关上线绑定消息
        Msg binding;    
        
        uint64_t taskId = 0;
        uint32_t actorEpoch = 0;
        bool failed = false;
        std::optional<Msg> enterRequest;
    };
    //存储当前 LogicThread 中“正在进行数据库加载、尚未完成正式上线”的玩家加载任务状态
    std::unordered_map<uint64_t,LoadState> playerLoads_;
    std::unordered_set<uint64_t> logoutPrepared_;
    std::atomic<bool> drainRequested_{false}, drainFinished_{false}, drainSucceeded_{false};
    int64_t drainDeadline_ = 0;
    bool draining_ = false;
    bool submitDbTask(Msg task);
};

#endif
