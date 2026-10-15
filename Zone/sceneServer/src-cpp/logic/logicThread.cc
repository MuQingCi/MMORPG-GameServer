#include "logic/logicThread.h"

#include "common/msg.h"
#include "common/msgBus.h"
#include "base/proto.h"
#include "db/dbValue.h"
#include "log/logger.h"
#include "lua/luaEnv.h"
#include "player/playerManager.h"
#include "proto/pb_lua.h"
#include "routeTable.h"
#include "logic/workerPushSink.h"
#include "service/aoi/aoiServiceRouter.h"
#include "service/aoi/aoiClientView.h"
#include <limits>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

// 生成的协议头（由 proto/CMakeLists.txt 生成 gs.* 消息）
#include "gameProto.pb.h"




/**
 *  逻辑线程的建立新连接指的是网络层连接的网关，而新建立连接的客户端则由网关处理然后将客户端数据如：玩家ID等打包发送到场景服并交由逻辑线程处理
 * 
 */



LogicThread::LogicThread(uint32_t threadId, 
                         MsgBus& bus, 
                         const Config& cfg, 
                         const AoiConfig& aoiCfg, 
                         const Map& map)
                       : threadId_(threadId), 
                         cfg_(cfg),
                         m_bus_(&bus),
                         players_(threadId),
                         workerPushSink_(bus, players_),
                         aoiService_(threadId,aoiCfg,workerPushSink_,map,
                              [&] {
                                  auto hooks = AoiServiceRouter::PublishHooks(threadId, bus.numWorker());
                                  hooks.processObserver = [](AoiService& service, EntityId observer) {
                                      return ProcessAoiClientObserver(service, observer);
                                  };
                                  return hooks;
                              }()),
                         playerService_(players_, aoiService_, map)
{
}

LogicThread::~LogicThread()
{
    stop();
}

PlayerManager& LogicThread::players()
{
    return players_;
}

size_t LogicThread::playerCount() const
{
    return players_.size();
}

bool LogicThread::start()
{
    if (started_.load(std::memory_order_acquire))
        return true;

    // Lua 环境必须在**本线程**创建：lua_State 与创建它的线程绑定使用
    apiCtx_.env = nullptr;
    apiCtx_.bus = m_bus_;
    apiCtx_.submitDbTask = [this](Msg task) { return submitDbTask(std::move(task)); };
    apiCtx_.players = &players_;
    apiCtx_.playerService = &playerService_;
    apiCtx_.timer = timer_;
    apiCtx_.workerId = threadId_;
    apiCtx_.redisNs = cfg_.redisNs;   // 脚本的 Redis 命令按此命名空间校验/分片

    started_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void LogicThread::stop()
{
    if(started_.load()) 
        requestDrain();
    if (thread_.joinable())
        thread_.join();
}

void LogicThread::requestDrain()
{
    drainRequested_.store(true);
}

void LogicThread::bindTimer(ITimerService* timer)
{
    timer_ = timer;
    apiCtx_.timer = timer;
}

void LogicThread::run()
{
    LOG_INFO << "logic thread start, worker=" << threadId_;

    // lua_State 在本线程内创建，保证 state 与线程一一对应
    lua_ = std::make_unique<LuaEnv>();
    lua_->setInstructionLimit(cfg_.luaInstructionLimit);
    if (!lua_->Init(cfg_.luaDir, &apiCtx_))
    {
        LOG_ERROR << "lua init failed, worker=" << threadId_ << " dir=" << cfg_.luaDir;
    }
    apiCtx_.env = lua_.get();

    lastFlushMs_ = NowMs();
    lastScanMs_ = NowMs();

    while (started_.load(std::memory_order_acquire))
    {
        // 安全点：热更只在"没有正在处理的消息"时发生
        doReloadIfNeeded();

        int64_t nowMs = NowMs();

        //线程停止时且不处于排空中则置排空状态为true并规定排空截至时间，清空players和playerLoads
        if(drainRequested_.load() && !draining_)
        {
            draining_=true;
            drainDeadline_=nowMs+cfg_.shutdownDrainMs;
            for(const auto& [id,state]:playerLoads_) 
            { 
                (void)state; 
                players_.remove(id); 
            }
            playerLoads_.clear();
        }

        if(draining_) 
        {
            //对每个玩家调用玩家登出方法
            std::vector<uint64_t> ids;
            players_.forEach([&](const Player& p) { 
                ids.push_back(p.id()); 
                return true; });

            for(auto id:ids) 
                logoutPlayer(id);
        } 
        else 
            //推进玩家载荷
            advancePlayerLoads();


        // 定期快照：防宕机丢数据（"只在登出时保存"是数据事故的常见来源）
        if (!draining_ && nowMs - lastFlushMs_ >= cfg_.flushIntervalMs)
        {
            lastFlushMs_ = nowMs;
            flushDirtyPlayers();
        }

        // AOI 周期更新
        aoiService_.OnTick(nowMs);
        advanceAoiRecovery(nowMs);

        persistence_.tick([this](Msg task) { return submitDbTask(std::move(task)); });
        pendingTasks_.tick(nowMs, [this](const Msg& task) { return m_bus_->sendToDb(task); });
        pendingTasks_.tickControls(nowMs, [this](const Msg& control) { return m_bus_->sendToDbControl(control); });

        if(draining_) 
        {
            //处于排空状态需要确保玩家清空和不存在待处理事件
            const bool finished=players_.size()==0 && !persistence_.unsettled() && !pendingTasks_.unsettled();
            if(finished || nowMs>=drainDeadline_) 
            {
                drainSucceeded_.store(finished);
                drainFinished_.store(true);
                if(!finished) 
                    LOG_ERROR << "shutdown persistence drain incomplete, worker=" << threadId_;
                break;
            }
        }

        //取事件并消费
        Msg m;
        // 单段等待：队列空则最多阻塞 idleWaitMs，非空立即取出
        if (!m_bus_->waitPopWorker(threadId_, m,
                                   std::chrono::milliseconds(cfg_.idleWaitMs)))
        {
            onIdle();
            continue;
        }

        if (m.head.msgType == MsgType::MSGTYPE_SHUTDOWN) { requestDrain(); continue; }

        const auto handleStart = std::chrono::steady_clock::now();
        handleMsg(m);
        const auto handleMicros = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - handleStart).count());
        busyMicros_.fetch_add(handleMicros, std::memory_order_relaxed);
        auto maxLoop = maxLoopMicros_.load(std::memory_order_relaxed);
        while (handleMicros > maxLoop &&
               !maxLoopMicros_.compare_exchange_weak(maxLoop, handleMicros,
                                                     std::memory_order_relaxed)) {}
        handledMessages_.fetch_add(1, std::memory_order_relaxed);
    }

    started_.store(false);
    if (lua_)
        lua_->Shutdown();
    LOG_INFO << "logic thread stopped, worker=" << threadId_
             << " handled=" << handledMessages_.load();
}

void LogicThread::handleMsg(Msg& m)
{
    switch (m.head.msgType)
    {
        case MsgType::MSGTYPE_CONN_NEW:
            onConnNew(m);
            break;
        case MsgType::MSGTYPE_CONN_DATA:
            onNetMsg(m);
            break;
        case MsgType::MSGTYPE_CONN_CLOSE:
            onConnClose(m);
            break;

        case MsgType::MSGTYPE_DB_RESULT_MYSQL:
        case MsgType::MSGTYPE_DB_RESULT_REDIS:
            onDbResult(m);
            break;
        case MSGTYPE_DB_STATUS:
            if (pendingTasks_.observeStatus(m) && m.body[0] == 0)
                LOG_WARNING << "DB task outcome unresolved, taskId=" << m.head.taskId;
            break;

        case MsgType::MSGTYPE_TIMER_FIRE:
            onTimer(m);
            break;

        case MsgType::MSGTYPE_RELOAD:
            onReload(m);
            break;

        case MSGTYPE_SERVICE:
            onService(m);
            break;
        default:
            LOG_DEBUG << "logic: unhandled msgType=" << MsgTypeName(m.head.msgType);
            break;
    }
}

void LogicThread::drainPending()
{
    Msg m;
    while (m_bus_->tryPopWorker(threadId_, m))
    {
        if (m.head.msgType != MsgType::MSGTYPE_SHUTDOWN)
            handleMsg(m);
    }
}

//TODO 旧设计遗留问题：把网关连接当场客户端连接
void LogicThread::onConnNew(Msg& m)
{
    // 连接建立只知道 session，还不知道 playerId：Actor 在第一条带 playerId 的
    // 业务消息到达时懒创建
    LOG_INFO << "worker " << threadId_ << " conn established, session=" << m.head.session
             << " peer=" << ServerIdName((uint8_t)m.head.ctx);
}

void LogicThread::onConnClose(Msg& m)
{
    LOG_INFO << "internal server connection closed, session=" << m.head.session;
    // 玩家离线由带完整绑定的 onNetMsg 通知处理，绝不根据 ConnClose 删除单个玩家。
}

bool LogicThread::logoutPlayer(uint64_t playerId)
{
    Player* p = players_.get(playerId);
    if(!p) return true;

    // 取消该玩家全部 Lua 定时器 + 通知脚本钩子
    if (lua_ && logoutPrepared_.insert(playerId).second)
        lua_->OnPlayerLogout(playerId);

    // 脚本钩子执行完仍需兜底离场。不能销毁 Player 后才生成 LEAVE。
    if(playerService_.LeaveScene(playerId) != PlayerService::Result::kOk) {
        LOG_ERROR << "AOI leave failed; preserving actor, pid=" << playerId;
        return false;
    }
    // 登出钩子执行后、Actor 销毁前冻结快照。固定数据
    // 独立存活，直到数据库返回明确成功结果。
    if (cfg_.persistenceEnabled && !persistence_.request(*p,true,
            [this](Msg task) { return submitDbTask(std::move(task)); })) {
        LOG_ERROR << "cannot retain final snapshot; preserving actor, pid=" << playerId;
        return false;
    }
    players_.remove(playerId);
    logoutPrepared_.erase(playerId);
    playerLoads_.erase(playerId);
    return true;
}

void LogicThread::onPlayerBinding(Msg& m)
{
    if(draining_) 
        return;
    //校验
    if(!m.head.hasClientBinding || !GatewayPlayer::Valid(m.head.clientBinding) ||
       m.head.clientBinding.playerId != m.head.playerId || m.head.session==0 || !m.body.empty()) 
        return;
    
    const auto& d=m.head.clientBinding;
    auto* p=players_.get(d.playerId);
    const auto latest=latestClientBindings_.find(d.playerId);
    if(m.head.Method==GatewayPlayer::kOnline) 
    {
        //有绑定记录
        if(latest!=latestClientBindings_.end()) 
        {
            const auto& old=latest->second;
            //校验新旧客户端数据
            if(d.gatewayId!=old.gatewayId || d.sceneId!=old.sceneId || d.clientEpoch<old.clientEpoch) 
                return;
            //校验客户端世代
            if(d.clientEpoch==old.clientEpoch) 
            {
                if(!p || !SameClientBinding(d,p->clientBinding()) || p->session()!=m.head.session) 
                    return;
                return; // 重复上线不清空基线、不提升 Actor epoch。
            }
        }

        //开启了持久化选项——将playerId → LoadState放入playerLoads_
        if(cfg_.persistenceEnabled) 
        {
            //找旧加载状态
            auto load=playerLoads_.find(d.playerId);
            
            //存在且绑定相似则直接返回
            if(load!=playerLoads_.end() && SameClientBinding(load->second.binding.head.clientBinding,d) &&
               load->second.binding.head.session==m.head.session) return;

            //存在就确保新旧相等
            if(load!=playerLoads_.end()) 
            {
                const auto& old=load->second.binding.head.clientBinding;
                if(old.gatewayId!=d.gatewayId || old.sceneId!=d.sceneId || d.clientEpoch<=old.clientEpoch) 
                    return;
            }

            if(!p || load!=playerLoads_.end()) 
            {
                if(load==playerLoads_.end() && playerLoads_.size()>=PlayerPersistence::kMaxPlayers) 
                {
                    LOG_ERROR << "player load capacity exhausted, pid=" << d.playerId;
                    return;
                }
                if(p) 
                    players_.renewEpoch(d.playerId);

                //创建新状态
                LoadState state; 
                state.binding=m;
                playerLoads_[d.playerId]=std::move(state);
                advancePlayerLoads();
                return;
            }
        }
        // DB 禁用或已有正常 Player 时的其他逻辑
        //获取玩家绑定的客户端世代
        const auto oldEpoch=p ? p->clientBinding().clientEpoch : 0;
        uint32_t actorEpoch;
        //获取/创建玩家
        p=players_.getOrCreate(d.playerId,m.head.session,actorEpoch);
        if(!p) 
            return;
        // 同一网关连接内客户端换绑也须使旧 Actor 异步任务失效。
        if(oldEpoch && p->clientBinding().clientEpoch!=d.clientEpoch)
            players_.renewEpoch(d.playerId);
        p->set_clientBinding(d); latestClientBindings_[d.playerId]=d;
        const auto id=aoiService_.EntityOf(d.playerId);
        if(id && aoiService_.IsEntityActive(*id)) 
        {
            if(aoiService_.views().findObserver(*id)) 
                aoiService_.OnPlayerRebind(d.playerId,oldEpoch,d.clientEpoch);
            else 
                aoiService_.OnPlayerOnline(d.playerId,d.gatewayId,d.clientEpoch);
        }
    } 
    else if(m.head.Method==GatewayPlayer::kOffline) {
        auto load=playerLoads_.find(d.playerId);
        if(load!=playerLoads_.end() && SameClientBinding(load->second.binding.head.clientBinding,d) &&
           load->second.binding.head.session==m.head.session) 
        {
            latestClientBindings_[d.playerId]=d;
            // 加载中的 Actor 未进场且没有业务修改，不保存默认数据。
            if(p && SameClientBinding(p->clientBinding(),d)) 
                players_.remove(d.playerId);

            playerLoads_.erase(load);
            return;
        }
        if(!p || p->session()!=m.head.session || !SameClientBinding(d,p->clientBinding())) 
            return;
        latestClientBindings_[d.playerId]=d;
        aoiService_.OnPlayerOffline(d.playerId,d.clientEpoch,0);
        logoutPlayer(d.playerId);
    }
}

Player* LogicThread::ensureActor(uint64_t playerId, uint64_t session, uint32_t& epochOut)
{
    return players_.getOrCreate(playerId, session, epochOut);
}

void LogicThread::advancePlayerLoads()
{
    for(auto& [id,state]:playerLoads_) 
    {
        //加载失败/taskId == 0 /阻塞登录则跳过
        if(state.failed || state.taskId || persistence_.blocksLogin(id)) 
            continue;

        //加载成功的则创建Player
        uint32_t epoch=0;
        auto* player=players_.getOrCreate(id,state.binding.head.session,epoch);
        if(!player) 
        { 
            state.failed=true; 
            continue; 
        }
        //创建成功就将client信息绑定到player并更新latestClientBindings_中的对应数据
        player->set_clientBinding(state.binding.head.clientBinding);
        latestClientBindings_[id]=state.binding.head.clientBinding;

        Msg request;
        request.head.msgType=MSGTYPE_DB_TASK_MYSQL;
        request.head.playerId=id; 
        request.head.epoch=epoch;
        request.head.Module=Module::SYS; 
        request.head.Method=Method::SYS_FLUSH;
        request.head.taskId=PendingDbTasks::newTaskId();
        request.body=PlayerSnapshotDao::buildLoad(id).Encode();

        if(submitDbTask(request)) 
        { 
            state.taskId=request.head.taskId; 
            state.actorEpoch=epoch; 
        }
    }
}

bool LogicThread::handlePlayerLoadResult(const Msg& message, const dbv::DbResult& result)
{
    auto it=playerLoads_.find(message.head.playerId);
    if(it==playerLoads_.end() || it->second.taskId!=message.head.taskId) 
        return false;

    auto& state=it->second;
    auto* player=players_.get(message.head.playerId);
    if(!player || player->epoch()!=state.actorEpoch ||
       !SameClientBinding(player->clientBinding(),state.binding.head.clientBinding)) 
        return true;
    
    PlayerSaveSnapshot snapshot; std::string error;
    if(!PlayerSnapshotDao::parseLoad(player->id(),result,snapshot,error) ||
       snapshot.sceneId!=state.binding.head.clientBinding.sceneId || !player->restoreSaveSnapshot(snapshot)) 
    {
        state.failed=true;
        LOG_ERROR << "player load failed, pid=" << message.head.playerId << " err=" << error;
        if(state.enterRequest) 
        { 
            sendRetTip(*state.enterRequest,1010,"player load failed"); 
            state.enterRequest.reset(); 
        }
        return true;
    }
    auto enter=std::move(state.enterRequest);
    playerLoads_.erase(it);
    if(enter) 
        onNetMsg(*enter);
    return true;
}

void LogicThread::sendRetTip(const Msg& request, int32_t code, const char* text)
{
    if (request.head.session == 0)
        return;

    gs::RetTip tip;
    tip.set_code(code);
    tip.set_text(text != nullptr ? text : "");

    Msg m;
    m.head.msgType = MsgType::MSGTYPE_SEND;
    // 非零 seq 是对客户端请求的回包：网关只接受原 module/method/playerId/seq。
    // 无关联号的通知仍使用 RetTip 的出站 SYS 路由。
    m.head.Module = request.head.seq ? request.head.Module : Module::SYS;
    m.head.Method = request.head.seq ? request.head.Method : Method::SYS_RET_TIP;
    m.head.seq = request.head.seq;
    m.head.session = request.head.session;
    m.head.playerId = request.head.playerId;
    m.body = tip.SerializeAsString();
    IsAccepted(m_bus_->sendToNet(std::move(m)));
}

void LogicThread::sendToSession(uint64_t session, const RouteTable::Route* route,
                                uint64_t seq, uint64_t playerId, const std::string& body)
{
    Msg m;
    m.head.msgType = MsgType::MSGTYPE_SEND;
    m.head.Module = route->Module;
    m.head.Method = route->Method;
    m.head.seq = seq;
    m.head.session = session;
    m.head.playerId = playerId;
    m.body = body;
    IsAccepted(m_bus_->sendToNet(std::move(m)));
}

void LogicThread::onNetMsg(Msg& m)
{
    if(draining_) 
        return;

    //新玩家加载走这个分支
    if(m.head.Module==GatewayPlayer::kModule) 
    { 
        onPlayerBinding(m); 
        return; 
    }

    const RouteTable::Route* route = RouteTable::FindC2S(m.head.Module, m.head.Method);

    // 顺序要求：先做路由校验，再创建 Actor
    // 对无效路由直接 return：不创建 Actor、也不刷新活跃度
    uint32_t epoch = 0;
    if (route == nullptr)
    {
        LOG_WARNING << "worker " << threadId_ << " route not found, Module=" << m.head.Module
                    << " Method=" << m.head.Method;
        sendRetTip(m, 1001, "route not found");
        return;
    }

    const auto loading=playerLoads_.find(m.head.playerId);
    if(loading!=playerLoads_.end()) 
    {
        auto& state=loading->second;
        if(!m.head.hasClientBinding || !SameClientBinding(m.head.clientBinding,state.binding.head.clientBinding) ||
           m.head.session!=state.binding.head.session) 
        { 
            sendRetTip(m,1006,"stale loading session"); 
            return; 
        }

        if(state.failed) 
        { 
            sendRetTip(m,1010,"player load failed"); 
            return; 
        }

        if(m.head.Module==Module::PLAYER && m.head.Method==Method::PLAYER_ENTER_SCENE && !state.enterRequest)
            state.enterRequest=m;
        else 
            sendRetTip(m,1009,"player loading or final save pending");
        return;
    }

    // 普通业务只使用已上线的当前绑定，不能隐式创建或重绑 Actor。
    if (m.head.playerId != 0)
    {
        Player* p = players_.get(m.head.playerId);
        if(!p || !m.head.hasClientBinding || p->session()!=m.head.session ||
           !SameClientBinding(p->clientBinding(),m.head.clientBinding)) 
        {
            sendRetTip(m,1006,"player session is not online");
            return;
        }
        epoch=p->epoch();
        p->touch(NowMs());
    }

    if (route->lua_fn == nullptr || route->lua_fn[0] == '\0')
    {
        if(m.head.Module == Module::PLAYER && m.head.Method == Method::PLAYER_ENTER_SCENE) 
        {
            gs::EnterSceneReq request;
            if(!request.ParseFromString(m.body)) 
            { 
                sendRetTip(m,1007,"invalid enter scene request"); return; 
            }

            auto* p = players_.get(m.head.playerId);
            if(!p) 
            { 
                sendRetTip(m,1006,"player session is not online"); return; 
            }

            const auto id = aoiService_.EntityOf(p->id());
            PlayerService::Result result = PlayerService::Result::kOk;
            if(!id || !aoiService_.IsEntityActive(*id))
                result = playerService_.EnterScene(p->id(),p->x(),p->y(),p->dir());

            gs::EnterSceneAck ack;
            if(result != PlayerService::Result::kOk) 
                ack.set_code(1008);
            else 
            {
                const auto entityId = aoiService_.EntityOf(p->id());
                ack.set_entity_id(*entityId);
                for(const auto& ent : aoiService_.BuildOwnerSnapshot().ents)
                    if(ent.entityId == *entityId) 
                        ack.set_generation(ent.entityGeneration);
                ack.set_x(p->x()); 
                ack.set_y(p->y()); 
                ack.set_dir(p->dir());
            }
            sendToSession(m.head.session,route,m.head.seq,m.head.playerId,ack.SerializeAsString());
            return;
        }
        if(m.head.Module == Module::PLAYER && m.head.Method == Method::PLAYER_LOGOUT) 
        {
            gs::LogoutReq request;
            if(!request.ParseFromString(m.body)) 
            { 
                sendRetTip(m,1007,"invalid logout request"); return; 
            }

            gs::LogoutAck ack;
            const auto* p=players_.get(m.head.playerId);
            if(p) 
                aoiService_.OnPlayerOffline(p->id(),p->clientBinding().clientEpoch,0);
            if(!logoutPlayer(m.head.playerId)) 
                ack.set_code(1008);
            sendToSession(m.head.session,route,m.head.seq,m.head.playerId,ack.SerializeAsString());
            return;
        }
        if (m.head.Module == Module::AOI && m.head.Method == Method::AOI_VIEW_RESYNC_REQ)
        {
            gs::ViewResyncReq request;
            if (!request.ParseFromString(m.body))
            {
                sendRetTip(m, 1007, "invalid view resync request");
                return;
            }
            const auto* observerPlayer = players_.get(m.head.playerId);
            const bool marked = observerPlayer != nullptr &&
                aoiService_.OnViewResyncReq(m.head.playerId,
                                             observerPlayer->clientBinding().clientEpoch);
            gs::ViewResyncAck ack;
            ack.set_code(marked ? 0 : 1008);
            sendToSession(m.head.session, route, m.head.seq, m.head.playerId, ack.SerializeAsString());
            return;
        }
        sendRetTip(m, 1002, "handler not implemented");
        return;
    }

    if (lua_ == nullptr || !lua_->ready())
    {
        sendRetTip(m, 1003, "lua env not ready");
        return;
    }

    const LuaEnv::DispatchResult r =
        lua_->DispatchC2S(route, m.head.session, m.head.playerId, m.head.seq, epoch, m.body);

    if (r != LuaEnv::DispatchResult::kOk)
    {
        // 明确回包，避免客户端无限等待
        sendRetTip(m,
                   r == LuaEnv::DispatchResult::kNoFn ? 1004 : 1005,
                   r == LuaEnv::DispatchResult::kNoFn ? "lua handler not found"
                                                      : "server internal error");
    }
}

void LogicThread::onDbResult(Msg& m)
{
    dbv::DbResult res;
    if (!dbv::DbResult::Decode(m.body, res))
    {
        LOG_WARNING << "worker " << threadId_ << " bad db result body";
        return;
    }

    // 在 Actor 校验前登记持久任务完成；绝不因超时重新执行 SQL。
    if (m.head.taskId && !pendingTasks_.complete(m)) 
    {
        pendingTasks_.acknowledgeDuplicate(m);
        pendingTasks_.tickControls(NowMs(), [this](const Msg& control) { 
            return m_bus_->sendToDbControl(control); });
        return;
    }
    // 在 Actor 处理前幂等登记完成结果；ACK 投递
    if(handlePlayerLoadResult(m,res)) 
        return;
    // 在回调处理后的下一轮主循环尝试执行。

    if (persistence_.owns(m.head.taskId)) 
    {
        if (!persistence_.complete(m.head.taskId,res,players_.get(m.head.playerId)))
            LOG_ERROR << "persistence result identity mismatch, taskId=" << m.head.taskId;
        else if (res.errcode || res.classification != dbv::ResultClass::kSuccess)
            LOG_ERROR << "save blocked pending recovery, pid=" << m.head.playerId << " err=" << res.errmsg;
        return;
    }

    // 1. 先校验 Actor 生命周期：发起查询时可能已下线/Actor 已重建
    if (m.head.playerId != 0 && !players_.validateEpoch(m.head.playerId, m.head.epoch))
    {
        droppedCallbacks_.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO << "worker " << threadId_ << " drop stale db result, pid=" << m.head.playerId
                 << " epoch=" << m.head.epoch << " ctx=" << m.head.ctx;
        return;
    }

    // 2. ctx == 0 表示"没有 Lua 回调"（例如存档任务），只记错误
    if (m.head.ctx == 0)
    {
        if (res.errcode != 0)
            LOG_WARNING << "worker " << threadId_ << " db task failed (no cb), err=" << res.errmsg;
        return;
    }

    // 必须由 owner 逻辑线程进 Lua（DB 线程绝不能碰 lua_State）
    if (lua_ != nullptr)
        lua_->DispatchDbAck((int64_t)m.head.ctx, res);
}

void LogicThread::onTimer(Msg& m)
{
    if(draining_) 
        return;
    if (m.head.Module == Module::TIMER && m.head.Method == Method::TIMER_LUA_FIRE)
    {
        // 逻辑层必须重新校验 Actor 是否还活着：TimerOp 里的 playerId/epoch是发起时刻的快照，不能当作"现在还存在"
        if (m.head.playerId != 0 &&
            !players_.validateEpoch(m.head.playerId, m.head.epoch))
        {
            droppedCallbacks_.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG << "worker " << threadId_ << " drop stale lua timer, pid="
                      << m.head.playerId << " timerId=" << m.head.ctx;
            return;
        }

        if (lua_ != nullptr)
            lua_->OnLuaTimerFire(m.head.ctx, m.head.epoch, m.head.playerId);
        return;
    }

    if (m.head.Module == Module::SYS)
    {
        if (m.head.Method == Method::SYS_FLUSH)
            flushDirtyPlayers();
        else if (m.head.Method == Method::SYS_SCAN_IDLE)
            onIdle();
        return;
    }

    LOG_DEBUG << "worker " << threadId_ << " unhandled timer, Module=" << m.head.Module
              << " Method=" << m.head.Method;
}

void LogicThread::onReload(Msg& m)
{
    // 热更广播：head.ctx = 目标版本号，body = 目标目录（目录即版本）
    // 用版本号而不是 bool 标志：bool 会被先完成的 worker 清掉，
    // 后到的 worker 永远看不到，导致各个 worker 脚本版本不一致。
    const uint64_t version = m.head.ctx;
    if (version <= pendingLuaVersion_.load(std::memory_order_acquire))
        return;

    pendingLuaDir_ = m.body;
    pendingLuaVersion_.store(version, std::memory_order_release);
    LOG_INFO << "worker " << threadId_ << " lua reload scheduled, version=" << version
             << " dir=" << pendingLuaDir_;
}


void LogicThread::onService(Msg& m)
{
    const auto result = AoiServiceRouter::Dispatch(aoiService_, threadId_, m_bus_->numWorker(), m);
    if(result == AoiServiceRouter::Result::kRejected) 
    {
        LOG_WARNING << "invalid internal service message, worker=" << threadId_
                    << " module=" << m.head.Module << " method=" << m.head.Method
                    << " sender=" << m.head.serviceSenderWorkerId;
        return;
    }

    if(result == AoiServiceRouter::Result::kSnapshotRequested) 
    {
        AoiWorkerPublishRequest reply;
        reply.msgType = MSGTYPE_SERVICE;
        reply.module = Module::AOI;
        reply.method = Method::AOI_OWNER_SNAPSHOT;
        reply.targets = {m.head.serviceSenderWorkerId};
        if(!AoiWire::EncodeSnapshot(aoiService_.BuildOwnerSnapshot(), reply.body)) 
        {
            LOG_ERROR << "AOI owner snapshot exceeds protocol limit or is invalid, worker=" << threadId_;
            return;
        }
        pendingAoiSnapshots_[m.head.serviceSenderWorkerId] = std::move(reply);
    } 
    else if(m.head.Method == Method::AOI_OWNER_SNAPSHOT) 
    {
        nextSnapshotRequestAt_.erase(m.head.serviceSenderWorkerId);
    }
}

void LogicThread::advanceAoiRecovery(int64_t now)
{
    if(now < nextAoiRecoveryAt_) 
        return;
    constexpr int64_t retryMs = 1000;

    nextAoiRecoveryAt_ = now > std::numeric_limits<int64_t>::max() - retryMs
        ? std::numeric_limits<int64_t>::max() : now + retryMs;
    
    //遍历待处理快照
    for(auto it = pendingAoiSnapshots_.begin(); it != pendingAoiSnapshots_.end();) 
    {
        //推送快照至其他逻辑线程
        const auto result = workerPushSink_.publishToWorkers(it->second);
        const auto accepted = std::find_if(result.deliveries.begin(), result.deliveries.end(),
            [&](const WorkerDeliveryResult& d) { return d.target == it->first && d.accepted; });
        if(accepted != result.deliveries.end()) 
            it = pendingAoiSnapshots_.erase(it);
        else ++it;
    }

    //清理已不需要恢复的请求时间
    const auto owners = aoiService_.OwnersNeedingSnapshot();
    for(auto it = nextSnapshotRequestAt_.begin(); it != nextSnapshotRequestAt_.end();) 
    {
        if(std::find(owners.begin(), owners.end(), it->first) == owners.end())
            it = nextSnapshotRequestAt_.erase(it);
        else ++it;
    }

    //为仍需恢复的 owner 发送请求
    for(auto owner : owners) 
    {
        if(owner >= m_bus_->numWorker() || owner == threadId_) 
            continue;
        const auto previous = nextSnapshotRequestAt_.find(owner);
        if(previous != nextSnapshotRequestAt_.end() && now < previous->second) 
            continue;
        AoiWorkerPublishRequest request;
        request.msgType = MSGTYPE_SERVICE;
        request.module = Module::AOI;
        request.method = Method::AOI_OWNER_SNAPSHOT_REQ;
        request.targets = {owner};
        request.body = AoiWire::SnapshotRequest();
        workerPushSink_.publishToWorkers(request);
        // 回复丢失也会重试；不能把请求入队成功当作恢复完成。
        nextSnapshotRequestAt_[owner] = nextAoiRecoveryAt_;
    }
}

void LogicThread::doReloadIfNeeded()
{
    const uint64_t target = pendingLuaVersion_.load(std::memory_order_acquire);
    if (target == 0 || target <= myLuaVersion_)
        return;

    if (lua_ == nullptr)
        return;

    // Reload 内部：先建新 state 并完整加载，成功才切换（失败回滚到旧 state）
    if (lua_->Init(pendingLuaDir_, &apiCtx_))
    {
        myLuaVersion_ = target;
        LOG_INFO << "worker " << threadId_ << " lua reloaded to version " << target
                 << " dir=" << pendingLuaDir_;
    }
    else
    {
        // 失败不回退版本号：下一轮还会重试（可能是目录还没同步完）
        LOG_ERROR << "worker " << threadId_ << " lua reload failed, keep old scripts";
    }
}

void LogicThread::onIdle()
{
    const int64_t now = NowMs();

    // 周期快照在 run() 的每轮主循环中调度，不能依赖队列变空。
    // 这里仅做空闲会话扫描：这里只做统计与告警，踢人策略交给脚本/配置决定。
    if (now - lastScanMs_ >= cfg_.scanIdleMs)
    {
        lastScanMs_ = now;

        size_t idle = 0;
        players_.forEach([&](const Player& p) {
            if (now - p.lastActiveMs() > 5 * 60 * 1000)
                ++idle;
            return true;
        });

        if (idle > 0)
            LOG_INFO << "worker " << threadId_ << " players=" << players_.size()
                     << " idle(>5min)=" << idle;
    }
}

void LogicThread::flushDirtyPlayers()
{
    if (!cfg_.persistenceEnabled) 
        return;
    players_.forEach([&](Player& p) 
    {
        //若正在加载或正处于线程结束的排空状态则返回
        if(playerLoads_.contains(p.id()) || draining_) 
            return true;

        if (p.dirty() && !persistence_.request(p,false,
                [this](Msg task) { 
                    return submitDbTask(std::move(task)); 
                }))
            LOG_ERROR << "cannot retain player snapshot, pid=" << p.id();
        return true;
    });
}

bool LogicThread::submitDbTask(Msg task)
{
    task.head.srcWorkerId = threadId_;
    return pendingTasks_.submit(std::move(task), NowMs(),
        [this](const Msg& request) { return m_bus_->sendToDb(request); });
}
