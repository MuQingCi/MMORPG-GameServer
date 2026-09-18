#ifndef CLEARMOON_LOGIC_LOGICTHREAD_H
#define CLEARMOON_LOGIC_LOGICTHREAD_H

#include "common/queue.h"
#include "common/msgBus.h"
#include "common/msg.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

struct lua_State;
class LuaEnv;
class ITimerService;
class PlayerManager;

class LogicThread
{
public:
    LogicThread(uint16_t threadId, MsgBus& bus, const std::string& luaDir);
    ~LogicThread();

    bool start();
    void stop();

    void bindTimer(ITimerService* timer);
    
private:
    void run();
    void handleMsg(Msg& m);

    void onNetMsg(Msg& m);
    void onDBResult(Msg& m);
    void onTimer(Msg& m);
    void onReload(Msg& m);
    void doReload();

    void ScanIdle();
    void FlushTimer();

    //viod RedisHeartbeat();

    void processPendingMsg();
    std::thread thread_;
    uint32_t threadId_;
    std::atomic<bool> started_ = false;
    std::atomic<bool> lua_reload_requested_ = false;

    MsgBus* m_bus_ = nullptr;
    std::string lua_dir_;

    std::unique_ptr<LuaEnv> luaEnvPtr_;
    std::unique_ptr<PlayerManager> playerMgr_;
};  

#endif