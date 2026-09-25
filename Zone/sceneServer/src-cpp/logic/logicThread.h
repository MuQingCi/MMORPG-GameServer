#ifndef CLEARMOON_LOGIC_LOGICTHREAD_H
#define CLEARMOON_LOGIC_LOGICTHREAD_H

#include "common/msg.h"
#include "common/msgBus.h"
#include "db/redisKey.h"
#include "lua/luaApiCtx.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

class LuaEnv;
class ITimerService;
class PlayerManager;
class Player;
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
    };

    LogicThread(uint32_t threadId, MsgBus& bus, const Config& cfg);
    ~LogicThread();

    LogicThread(const LogicThread&) = delete;
    LogicThread& operator=(const LogicThread&) = delete;

    bool start();
    void stop();

    // 由 SceneServer 在 start 前注入（LuaEnv 未绑定时 timer API 会明确报错而不是野指针崩溃）
    void bindTimer(ITimerService* timer);

    uint32_t threadId() const { return threadId_; }
    PlayerManager& players();   // 定义在 .cc：头文件里 PlayerManager 还是不完整类型
    size_t playerCount() const; // 同样定义在 .cc

    // 统计（监控用）
    uint64_t handledMessages() const { return handledMessages_.load(std::memory_order_relaxed); }
    uint64_t droppedCallbacks() const { return droppedCallbacks_.load(std::memory_order_relaxed); }

private:
    void run();
    void handleMsg(Msg& m);

    void onConnNew(Msg& m);
    void onConnClose(Msg& m);
    void onNetMsg(Msg& m);
    void onDbResult(Msg& m);
    void onTimer(Msg& m);
    void onReload(Msg& m);

    void onIdle();             // 空闲内务：定期快照 / 空闲扫描
    void doReloadIfNeeded();   // 主循环安全点热更
    void drainPending();

    // 懒创建 Actor：第一条带 playerId 的业务消息就是"玩家进入本场景"
    Player* ensureActor(uint64_t playerId, uint64_t session, uint32_t& epochOut);

    void sendRetTip(uint64_t session, uint64_t playerId, uint64_t seq, int32_t code, const char* text);
    void sendToSession(uint64_t session, const RouteTable::Route* route, uint64_t seq,
                       uint64_t playerId, const std::string& body);
    void flushDirtyPlayers();

    std::thread thread_;
    uint32_t threadId_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopRequested_{false};

    MsgBus* m_bus_ = nullptr;
    Config cfg_;

    std::unique_ptr<LuaEnv> lua_;
    std::unique_ptr<PlayerManager> players_;
    LuaApiCtx apiCtx_;

    ITimerService* timer_ = nullptr;

    // 热更：目标版本 + 目标目录（由广播消息带来，每个 worker 独立应用）
    std::atomic<uint64_t> pendingLuaVersion_{0};
    std::string pendingLuaDir_;
    uint64_t myLuaVersion_ = 0;

    int64_t lastFlushMs_ = 0;
    int64_t lastScanMs_ = 0;

    std::atomic<uint64_t> handledMessages_{0};
    std::atomic<uint64_t> droppedCallbacks_{0};
};

#endif
