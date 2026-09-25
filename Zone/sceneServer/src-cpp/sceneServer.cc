#include "sceneServer.h"

#include "common/msg.h"
#include "db/dbThread.h"
#include "log/logger.h"
#include "logic/logicThread.h"
#include "net/net_server.h"
#include "player/playerManager.h"
#include "timer/TimerThread.h"

#include <sstream>
#include <utility>

SceneServer::SceneServer(const SceneConfig& cfg)
    : cfg_(cfg), luaDir_(cfg.luaDir)
{
    // bus 最先创建：net/timer/db/logic 全都依赖它。
    // Redis 分片数必须在这里定下来：MsgBus 的 Redis 队列数 = 分片数，
    // 而 db.redis 绑定层算出的分片索引会直接用作队列下标（不一致会越界/错投）。
    bus_ = std::make_unique<MsgBus>(cfg_.logicThreadNum, cfg_.zone.db.redis.ShardCount());
}

SceneServer::~SceneServer()
{
    stop();
}

bool SceneServer::start()
{
    if (started_.load(std::memory_order_acquire))
        return true;

    // 从这里开始就算"已启动"：任何中途失败都保证析构/stop() 能把已创建的部分停干净
    started_.store(true, std::memory_order_release);

    // ---------------- 1. 网络线程 ----------------
    netServer_ = std::make_unique<NetServer>(cfg_.net, *bus_);
    if (!netServer_->start())
    {
        LOG_ERROR << "net server start failed";
        return false;
    }

    // ---------------- 2. 定时器线程（全场景唯一）----------------
    timerThread_ = std::make_unique<TimerThread>(*bus_, cfg_.timerTickMs, cfg_.timerWheelSlots);
    if (!timerThread_->start())
    {
        LOG_ERROR << "timer thread start failed";
        netServer_->stop();
        return false;
    }

    // ---------------- 3. DB 线程组（MySQL / Redis 两条通道，Redis 可多实例分片）----------------
    if (cfg_.zone.dbEnable)
    {
        dbThread_ = std::make_unique<DBThread>(*bus_, cfg_.zone.db);
        if (!dbThread_->start())
        {
            LOG_ERROR << "db threads start failed";
            timerThread_->stop();
            netServer_->stop();
            return false;
        }
    }
    else
    {
        LOG_WARNING << "db channel disabled by config (ZoneServer.db.enable=false)";
    }

    // ---------------- 4. 逻辑线程池 ----------------
    const LogicThread::Config logicCfg = cfg_.MakeLogicConfig();
    logicThreads_.reserve(cfg_.logicThreadNum);
    for (uint32_t i = 0; i < cfg_.logicThreadNum; ++i)
    {
        auto lt = std::make_unique<LogicThread>(i, *bus_, logicCfg);
        // ★ 必须注入定时器服务：否则 Lua 里的 timer.after 会拿到空服务
        //   （旧实现里 timer_ 既无默认值又无人 bindTimer -> 野指针崩溃）
        lt->bindTimer(timerThread_.get());
        if (!lt->start())
        {
            LOG_ERROR << "logic thread " << i << " start failed";
            stop();
            return false;
        }
        logicThreads_.push_back(std::move(lt));
    }

    // ---------------- 5. 反向连接网关集群 ----------------
    // 设计文档 3.1.3：场景服启动时主动连网关（可连多个做冗余），
    // 而不是等网关拿连接池来连 —— 否则响应无法与请求匹配、回程地址无法表达。
    // 网关列表来自 ZoneConfig（区服共享：场景服/全局服/聊天服连同一批）。
    for (const auto& peer : cfg_.zone.gateways)
    {
        if (!netServer_->connectTo(peer))
            LOG_WARNING << "connect gateway failed: " << peer.addr << ":" << peer.port;
    }

    LOG_INFO << "scene server started, zoneId=" << cfg_.zone.zoneId << " sceneId=" << cfg_.sceneId
             << " logicThreads=" << cfg_.logicThreadNum << " luaDir=" << luaDir_
             << " dbEnable=" << (cfg_.zone.dbEnable ? 1 : 0)
             << " redisShards=" << cfg_.zone.db.redis.ShardCount();
    return true;
}

void SceneServer::stop()
{
    // 幂等：析构与显式 stop 都会走到这里；
    // 也覆盖"start() 中途失败"的路径（此时也要把已创建的部分停干净）
    if (stopped_.exchange(true, std::memory_order_acq_rel))
        return;

    // 逆序停止：先切断输入（网络），再停消费者（逻辑），最后停辅助线程
    if (netServer_)
        netServer_->stop();          // 不再有新消息进来

    for (auto& lt : logicThreads_)
    {
        if (lt)
            lt->stop();
    }

    if (timerThread_)
    {
        timerThread_->stop();        // 逻辑线程已停，不会再有新的 addTimer
        timerThread_->join();
    }

    if (dbThread_)
        dbThread_->stop();

    // logic/timer/db 都停了才会走到这里，此时 bus 上不会再有生产者
    LOG_INFO << "scene server stopped";
}

void SceneServer::reloadLua(const std::string& newDir)
{
    if (newDir.empty())
        return;

    const uint64_t version = luaVersion_.fetch_add(1, std::memory_order_acq_rel) + 1;
    luaDir_ = newDir;

    Msg m;
    m.head.msgType = MsgType::MSGTYPE_RELOAD;
    m.head.ctx = version;   // 版本号由广播携带，每个 worker 独立应用
    m.body = newDir;
    bus_->broadcastToLogic(m);

    LOG_INFO << "lua reload broadcast, version=" << version << " dir=" << newDir;
}

size_t SceneServer::playerCount() const
{
    size_t total = 0;
    for (const auto& lt : logicThreads_)
    {
        if (lt)
            total += lt->playerCount();
    }
    return total;
}

std::string SceneServer::stats() const
{
    std::ostringstream oss;
    oss << "logicThreads=" << logicThreads_.size();
    for (const auto& lt : logicThreads_)
    {
        if (!lt)
            continue;
        oss << " [w" << lt->threadId() << " handled=" << lt->handledMessages()
            << " players=" << lt->playerCount()
            << " dropped=" << lt->droppedCallbacks() << "]";
    }
    if (timerThread_)
        oss << " timer(fired=" << timerThread_->firedCount() << " live=" << timerThread_->liveCount() << ")";
    if (dbThread_)
    {
        oss << " db(mysql=" << dbThread_->mysqlTasks() << " redis=" << dbThread_->redisTasks()
            << " errors=" << dbThread_->errors() << " redisShards=" << dbThread_->redisShardCount();
        for (size_t i = 0; i < dbThread_->redisShardCount(); ++i)
            oss << (dbThread_->redisConnected(i) ? " up" : " down");
        oss << ")";
    }
    return oss.str();
}
