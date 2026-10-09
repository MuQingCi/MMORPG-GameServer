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
#include <limits>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

// 生成的协议头（由 proto/CMakeLists.txt 生成 gs.* 消息）
#include "gameProto.pb.h"

namespace
{
// 落库 SQL：必须幂等（超时重试一定会发生）
// 生产上这属于 DAO 层的职责，这里给出最小可用版本作为占位与示例
constexpr const char* kSavePlayerSql =
    "INSERT INTO player(id,hp,mp,gold,pos_x,pos_y,dir,updated_at) VALUES(?,?,?,?,?,?,?,NOW()) "
    "ON DUPLICATE KEY UPDATE hp=VALUES(hp),mp=VALUES(mp),gold=VALUES(gold),"
    "pos_x=VALUES(pos_x),pos_y=VALUES(pos_y),dir=VALUES(dir),updated_at=NOW()";

std::string I64ToStr(int64_t v) { return std::to_string(v); }
}  // namespace


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
                         m_bus_(&bus), 
                         cfg_(cfg),
                         players_(threadId),
                         workerPushSink_(bus, players_),
                          aoiService_(threadId,aoiCfg,workerPushSink_,map,
                              AoiServiceRouter::PublishHooks(threadId, bus.numWorker())),
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
    if (!started_.exchange(false, std::memory_order_acq_rel))
        return;

    stopRequested_.store(true, std::memory_order_release);

    // 投一条 SHUTDOWN 把阻塞在 waitPopWorker 的线程叫醒
    Msg m;
    m.head.msgType = MsgType::MSGTYPE_SHUTDOWN;
    m_bus_->sendToWorker(threadId_, std::move(m));

    if (thread_.joinable())
        thread_.join();
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

        //AOITick
        int64_t nowMs = NowMs();
        aoiService_.OnTick(nowMs);
        advanceAoiRecovery(nowMs);

        Msg m;
        // 单段等待：队列空则最多阻塞 idleWaitMs，非空立即取出
        if (!m_bus_->waitPopWorker(threadId_, m,
                                   std::chrono::milliseconds(cfg_.idleWaitMs)))
        {
            onIdle();
            continue;
        }

        if (m.head.msgType == MsgType::MSGTYPE_SHUTDOWN)
            break;

        handleMsg(m);
        handledMessages_.fetch_add(1, std::memory_order_relaxed);
    }

    drainPending();
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
    const uint64_t playerId = m.head.playerId;
    LOG_INFO << "worker " << threadId_ << " conn closed, session=" << m.head.session
             << " playerId=" << playerId;

    if (playerId == 0)
        return;   // 会话级连接（尚未绑定玩家）：无 Actor 需要清理

    Player* p = players_.get(playerId);
    if (p == nullptr)
        return;   // 该玩家不在本分片（或已下线）：与本线程无关
    if(p->session() != m.head.session)
        return; // 旧网关连接关闭，不能删除已经换绑的新 Actor。

    // 顺序很重要：先落盘 -> 再取消定时器 -> 最后销毁 Actor
    if (p->dirty())
    {
        dbv::DbTask task;
        task.kind = 2;
        task.args.push_back(kSavePlayerSql);
        task.args.push_back(I64ToStr((int64_t)p->id()));
        task.args.push_back(I64ToStr(p->hp()));
        task.args.push_back(I64ToStr(p->mp()));
        task.args.push_back(I64ToStr((int64_t)p->gold()));
        task.args.push_back(I64ToStr(p->x()));
        task.args.push_back(I64ToStr(p->y()));
        task.args.push_back(I64ToStr(p->dir()));

        Msg db;
        db.head.msgType = MsgType::MSGTYPE_DB_TASK_MYSQL;
        db.head.Module = Module::SYS;
        db.head.playerId = p->id();
        db.head.session = p->session();
        db.head.epoch = p->epoch();
        db.head.srcWorkerId = threadId_;   // 结果回来也只回到本线程（且不会进 Lua）
        db.body = task.Encode();
        m_bus_->sendToDb(std::move(db));
    }

    // 取消该玩家全部 Lua 定时器 + 通知脚本钩子
    if (lua_)
        lua_->OnPlayerLogout(playerId);

    // 脚本钩子执行完仍需兜底离场。不能销毁 Player 后才生成 LEAVE。
    if(playerService_.LeaveScene(playerId) != PlayerService::Result::kOk) {
        LOG_ERROR << "AOI leave failed; preserving actor, pid=" << playerId;
        return;
    }
    players_.remove(playerId);
}

Player* LogicThread::ensureActor(uint64_t playerId, uint64_t session, uint32_t& epochOut)
{
    return players_.getOrCreate(playerId, session, epochOut);
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
    m_bus_->sendToNet(std::move(m));
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
    m_bus_->sendToNet(std::move(m));
}

void LogicThread::onNetMsg(Msg& m)
{
    const RouteTable::Route* route = RouteTable::FindC2S(m.head.Module, m.head.Method);

    // 顺序要求：先做路由校验，再创建 Actor
    // 现在对无效路由直接 return：不创建 Actor、也不刷新活跃度
    uint32_t epoch = 0;

    if (route == nullptr)
    {
        LOG_WARNING << "worker " << threadId_ << " route not found, Module=" << m.head.Module
                    << " Method=" << m.head.Method;
        sendRetTip(m, 1001, "route not found");
        return;
    }

    // 懒创建/更新 Actor：带 playerId 的消息就是"该玩家在本场景的一条指令"，由它来保证"同一个玩家的所有消息落在同一个线程、串行处理"

    // 落点说明：这里仍在 lua_fn / lua ready 校验之前，即"路由存在"就会创建 Actor
    //若希望更严格（只有确实能分发出去才创建），
    // 把这一段整体下移到下面的"lua env not ready"校验之后即可 —— 行为差异仅出现在「路由存在但处理器未实现 / Lua 未就绪」这两种情况
    if (m.head.playerId != 0)
    {
        Player* p = ensureActor(m.head.playerId, m.head.session, epoch);
        if (p != nullptr)
            p->touch(NowMs());
    }

    if (route->lua_fn == nullptr || route->lua_fn[0] == '\0')
    {
        // TODO C++ 处理的路由（例如内部系统方法），当前未实现
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

    // 定期快照：防宕机丢数据（"只在登出时保存"是数据事故的常见来源）
    if (now - lastFlushMs_ >= cfg_.flushIntervalMs)
    {
        lastFlushMs_ = now;
        flushDirtyPlayers();
    }

    // 空闲会话扫描：这里只做统计与告警，踢人策略交给脚本/配置决定
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
    if (players_.size() == 0)
        return;

    size_t saved = 0;
    players_.forEach([&](Player& p) {
        if (!p.dirty())
            return true;

        dbv::DbTask task;
        task.kind = 2;   // exec
        task.args.push_back(kSavePlayerSql);
        task.args.push_back(I64ToStr((int64_t)p.id()));
        task.args.push_back(I64ToStr(p.hp()));
        task.args.push_back(I64ToStr(p.mp()));
        task.args.push_back(I64ToStr((int64_t)p.gold()));
        task.args.push_back(I64ToStr(p.x()));
        task.args.push_back(I64ToStr(p.y()));
        task.args.push_back(I64ToStr(p.dir()));

        Msg db;
        db.head.msgType = MsgType::MSGTYPE_DB_TASK_MYSQL;
        db.head.Module = Module::SYS;
        db.head.Method = Method::SYS_FLUSH;
        db.head.playerId = p.id();
        db.head.session = p.session();
        db.head.epoch = p.epoch();
        db.head.srcWorkerId = threadId_;
        db.head.ctx = 0;              // 无 Lua 回调
        db.body = task.Encode();

        // 入队成功才清脏标记：失败时保留脏标记，下个周期再存
        if (m_bus_->sendToDb(std::move(db)))
        {
            p.clearDirty();
            ++saved;
        }
        return true;
    });

    if (saved > 0)
        LOG_INFO << "worker " << threadId_ << " flushed " << saved << " players";
}
