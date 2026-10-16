#include "sceneServer.h"

#include "common/msg.h"
#include "db/dbThread.h"
#include "log/logger.h"
#include "logic/logicThread.h"
#include "net/net_server.h"
#include "player/playerManager.h"
#include "timer/TimerThread.h"
#include "metrics/metricsJson.h"

#include <sstream>
#include <fstream>
#include <cstdio>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <utility>

SceneServer::SceneServer(const SceneConfig& cfg)
    : cfg_(cfg), metrics_(cfg.logicThreadNum), luaDir_(cfg.luaDir)
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

    std::string mapError;
    if(!Map::Load(cfg_.mapPath, cfg_.aoi.cellSize, map_, mapError)) {
        LOG_ERROR << "map load failed: " << mapError;
        return false;
    }

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
        auto lt = std::make_unique<LogicThread>(i, *bus_, logicCfg, cfg_.aoi, map_, metrics_.worker(i));
        //  必须注入定时器服务：否则 Lua 里的 timer.after 会拿到空服务
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
    for (const auto& configuredPeer : cfg_.zone.gateways)
    {
        for (uint32_t channel = 0; channel < cfg_.zone.backendChannels; ++channel)
        {
            PeerConfig peer = configuredPeer;
            peer.channelId = channel;
            if (!netServer_->connectTo(peer))
                LOG_WARNING << "connect gateway failed: gatewayId=" << peer.gatewayId
                            << " channel=" << channel << " " << peer.addr << ":" << peer.port;
        }
    }

    {
        metricsStart_ = std::chrono::steady_clock::now();
        sampleMetrics();
        writeMetrics(); // metricsPath 为空时仅维护内存快照，不写文件。
        metricsThread_ = std::thread([this] {
            while (started_.load(std::memory_order_acquire))
            {
                std::unique_lock<std::mutex> waitLock(metricsWaitMutex_);
                metricsWaitCv_.wait_for(waitLock, std::chrono::seconds(1), [this] {
                    return !started_.load(std::memory_order_acquire);
                });
                waitLock.unlock();
                if (started_.load(std::memory_order_acquire))
                {
                    sampleMetrics();
                    writeMetrics();
                }
            }
            sampleMetrics();
            writeMetrics();
        });
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

    started_.store(false, std::memory_order_release);
    metricsWaitCv_.notify_all();
    if (metricsThread_.joinable())
        metricsThread_.join();

    // 逆序停止：先切断输入（网络），再停消费者（逻辑），最后停辅助线程
    if (netServer_)
        netServer_->stop();          // 不再有新消息进来

    for (auto& lt : logicThreads_)
        if(lt) lt->requestDrain();

    for (auto& lt : logicThreads_)
    {
        if (lt) {
            lt->stop();
            if (!lt->drainFinished()) metrics_.worker(lt->threadId()).setLifecycle(WorkerLifecycle::Failed);
            if(!lt->drainSucceeded()) LOG_ERROR << "worker shutdown not durably drained, worker=" << lt->threadId();
        }
    }

    if (timerThread_)
    {
        timerThread_->stop();        // 逻辑线程已停，不会再有新的 addTimer
        timerThread_->join();
    }

    if (dbThread_)
        dbThread_->stop();

    if (!logicThreads_.empty())
    {
        sampleMetrics();
        writeMetrics(); // 最终采样总是执行；文件导出仍由 metricsPath 控制。
    }

    // logic/timer/db 都停了才会走到这里，此时 bus 上不会再有生产者
    LOG_INFO << "scene server stopped";
}

void SceneServer::writeMetrics() const
{
    if (cfg_.metricsPath.empty())
        return;
    const std::string tmp = cfg_.metricsPath + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return;
    const SceneMetricsSnapshot snapshot = metricsSnapshot();
    out << serializeSceneMetrics(snapshot);
    out.close();
    ::rename(tmp.c_str(), cfg_.metricsPath.c_str());
}

void SceneServer::sampleMetrics()
{
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    std::vector<uint64_t> depths;
    uint64_t netDepth = 0;
    if (bus_)
    {
        const auto busDepths = bus_->workerDepths();
        depths.assign(busDepths.begin(), busDepths.end());
        netDepth = bus_->netDepth();
        for (const auto& thread : logicThreads_)
        {
            if (!thread) continue;
            const auto id = thread->threadId();
            metrics_.worker(id).setEnqueued(bus_->enqueuedBusinessCount(id), bus_->enqueuedInternalCount(id));
        }
    }
    std::lock_guard lock(metricsSnapshotMutex_);
    metricsSnapshot_ = metricsSampler_.sample(metrics_, now, depths, netDepth);
    const auto startMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        metricsStart_.time_since_epoch()).count());
    metricsSnapshot_.uptimeMs = now >= startMs ? now - startMs : 0;
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
    const auto sampledMetrics = metricsSnapshot();
    std::ostringstream oss;
        oss << "logicThreads=" << sampledMetrics.workers.size()
        << " netQueue=" << sampledMetrics.netQueueDepth;
    for (size_t workerId = 0; workerId < sampledMetrics.workers.size(); ++workerId)
    {
        const auto& sampled = sampledMetrics.workers[workerId];
        oss << " [w" << workerId << " handled=" << sampled.handled
            << " players=" << sampled.players
            << " dropped=" << sampled.dropped
            << " queue=" << sampled.queueDepth
            << " busyUs=" << sampled.busyUs
            << " maxLoopUs=" << sampled.maxLoopUs
            << " health=" << static_cast<unsigned>(sampled.health)
            << " activePermille=" << sampled.activePermille << "]";
    }
    return oss.str();
}

SceneMetricsSnapshot SceneServer::metricsSnapshot() const
{
    std::lock_guard lock(metricsSnapshotMutex_);
    return metricsSnapshot_;
}
