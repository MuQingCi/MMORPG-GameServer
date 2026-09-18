#include "logicThread.h"
#include "log/logger.h"
#include "lua/luaEnv.h"
#include "common/msg.h"
#include "common/msgBus.h"
#include "player/playerMannager.h"
#include "routeTable.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <lauxlib.h>
#include <memory>
#include <thread>
#include <utility>

static const std::chrono::milliseconds kIdleWaitMs = std::chrono::milliseconds(3); 

namespace {
// 统一回一个 RetTip(通用错误提示), 避免客户端无限等待
void SendRetTip(uint64_t session, uint64_t playerId,
                uint64_t seq, int32_t code,
                const char* text, MsgBus* mBus) 
{
    if (session == 0) return;
    gameSence::RetTip tip;
    tip.set_code(code);
    tip.set_text(text ? text : "");
    Msg m;
    m.head.session = session;
    m.head.seq     = seq;
    m.head.playerId= playerId;
    m.body         = tip.SerializeAsString();
    mBus->sendToNet(m);
}
}  // namespace

LogicThread::LogicThread(uint16_t threadId, 
                         MsgBus& bus,
                         const std::string& luaDir)
                        :threadId_(threadId),
                         m_bus_(&bus),
                         lua_dir_(luaDir),
                         luaEnvPtr_(std::make_unique<LuaEnv>(luaDir)),
                         playerMgr_(std::make_unique<PlayerManager>())
{
    luaEnvPtr_->Init(lua_dir_);
}

LogicThread::~LogicThread()
{
    stop();
}

bool LogicThread::start()
{
    if(started_) return true;

    thread_ = std::thread([this]{run();});
    started_ = true;
    return true;
}

void LogicThread::stop()
{
    if (!started_.exchange(false)) return;

    Msg m;
    m.head.msgType = MsgType::MSGTYPE_SHUTDOWN;
    m_bus_->sendToWorker(threadId_, m);      //唤醒阻塞中的线程

    if(thread_.joinable())
        thread_.join();
}

void LogicThread::bindTimer(ITimerService* timer)
{
    luaEnvPtr_->bindTimer(timer);
}

void LogicThread::run()
{
    Msg m;
    while (started_.load(std::memory_order_acquire)) 
    {
        //热更新
        if (lua_reload_requested_.exchange(false,std::memory_order_acq_rel)) 
            doReload();
        
        if(m_bus_->tryPopWorker(threadId_, m))
        {
            if (m.head.msgType == MsgType::MSGTYPE_SHUTDOWN) 
                break;
            handleMsg(m);
            continue;
        }    
        
        //TODO 可以实现阻塞队列
        m_bus_->waitWorker(threadId_, kIdleWaitMs);
    }

    processPendingMsg();
}

void LogicThread::handleMsg(Msg& m)
{
    switch (m.head.msgType) 
    {
    //Net
    case MsgType::MSGTYPE_CONN_NEW:
        break;
    case MsgType::MSGTYPE_CONN_DATA:
        onNetMsg(m);
        break;
    case MsgType::MSGTYPE_CONN_CLOSE:
        break;

    //DB
    case MsgType::MSGTYPE_DB_TASK:
        m_bus_->sendToDB(m);
        break;
    case MsgType::MSGTYPE_DB_RESULT:
        onDBResult(m);
        break;
    
    case MsgType::MSGTYPE_TIMER_FIRE:
        onTimer(m);
        break;
    
    case MsgType::MSGTYPE_RELOAD:
        onReload(m);
        break;
    default:
        break;
    }
}


void LogicThread::onNetMsg(Msg& m)
{
    //TODO 根据消息查路由表并处理
    auto route = RouteTable::FindC2S(m.head.Module, m.head.Method);
    if(route == nullptr)
    {
        LOG_WARNING<<"route:(" << m.head.Module << "," << m.head.Method <<") not find";
        
        Msg sendMsg;
        sendMsg.head.msgType  = MsgType::MSGTYPE_SEND;
        sendMsg.head.seq      = m.head.seq;
        sendMsg.head.session  = m.head.session;
        sendMsg.head.playerId = m.head.playerId;
        sendMsg.body.append("Invalid Method!");
        
        m_bus_->sendToNet(std::move(sendMsg));
        return;
    }

    auto dispatchResult  = luaEnvPtr_->DispatchC2S(route->lua_fn, m.head.session, m.head.playerId, -1);
    
    if(dispatchResult != LuaEnv::DispatchResult::kOk)
    {
        // 脚本缺失/执行出错: 明确回包, 避免客户端无限等待
        SendRetTip(m.head.session, m.head.playerId, 
                   m.head.seq,
                   dispatchResult == LuaEnv::DispatchResult::kNoFn ? 1001 : 1002,
                   dispatchResult == LuaEnv::DispatchResult::kNoFn ? "处理函数不存在"
                                                                        : "服务器内部错误", 
                   m_bus_);
    }
}

void LogicThread::onDBResult(Msg& m)
{

}

void LogicThread::onTimer(Msg& m)
{
    if (m.head.Module == Module::SYS) 
    {
        if (m.head.Method == Method::SYS_SCAN_IDLE) 
            ScanIdle();
        else if (m.head.Method == Method::SYS_FLUSH) 
            FlushTimer();
        // else if (m.head.Method == SYS_REDIS_HEART) 
        //     RedisHeartbeat();
	} 
    else if (m.head.Module == Module::TIMER && m.head.Method == Method::TIMER_LUA_FIRE) 
		    luaEnvPtr_->OnLuaTimerFire(m.head.ctx);
    else if (m.head.Module == Module::ENEMY)
    {
        //TODO 敌人模块
		// 敌人内部定时器: ctx 承载 eid(见 SceneEnemyMgr::RegisterRepeatingTimer)
		// if (m.head.Method == Method::ENEMY_AI_TICK)
		// 	SceneEnemyMgr::I().OnAiTick((uint32_t)m.head.ctx);
		// else if (m.head.Method == Method::E_SCENE_RESTART)
		// 	SceneEnemyMgr::I().OnSceneRestart((int32_t)m.head.ctx);
	} 
    else
    {
		LOG_DEBUG<<"timer fire unhandled Module="<< m.head.Module<< ", Method="<< m.head.Method;
	}
}

void LogicThread::onReload(Msg& m)
{
    //TODO 校验角色是否为GM,是则置lua_reload_requested_为true;待处理消息结束后调用doReload进行热更
}

void LogicThread::doReload()
{
    luaEnvPtr_->Reload();
}

void LogicThread::ScanIdle()
{
    //TODO 待完善实现
}

void LogicThread::FlushTimer()
{
    //TODO 待完善实现
}


void LogicThread::processPendingMsg()
{
    Msg m;
    while (m_bus_ && m_bus_->tryPopWorker(threadId_, m)) 
    {
        handleMsg(m);
    }
}