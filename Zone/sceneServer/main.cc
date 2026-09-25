// ============================================================================
// main.cc —— 场景服进程入口
//
// 启动顺序（每一步失败都必须能明确报出来，而不是带病启动）：
//   1. 加载配置：**两份**
//        - zoneConfig.yaml  : 区服共享（zoneId / MySQL / Redis / 网关 / 日志）
//        - sceneConfig.yaml : 场景服特有（sceneId / 逻辑线程 / Lua / net / timer）
//      两者由 SceneConfig::AttachZone() 合并并派生（net.zoneId、Redis 命名空间等）。
//   2. 初始化日志（用区服统一的日志策略）
//   3. RouteTable::Validate() —— 校验 (Module,Method) 不重复、
//      req_type/ack_type 都能在 protobuf 里解析。这是防止"路由表与 proto 漂移"
//      最有效的一道防线，失败直接 exit(1)；
//   4. SceneServer 组装与启动
//   5. 主线程只做信号等待：SIGINT/SIGTERM = 优雅退出，SIGUSR1 = 热更 Lua 脚本
// ============================================================================
#include "config/sceneConfig.h"
#include "config/zoneConfig.h"
#include "log/asyncLogger.h"
#include "log/logger.h"
#include "routeTable.h"
#include "sceneServer.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace
{
LogLevel ParseLogLevel(const std::string& s)
{
    if (s == "debug") return LogLevel::DEBUG;
    if (s == "info") return LogLevel::INFO;
    if (s == "warn" || s == "warning") return LogLevel::WARNING;
    if (s == "error") return LogLevel::ERROR;
    return LogLevel::INFO;
}

void EnsureDir(const std::string& dir)
{
    if (!dir.empty())
        ::mkdir(dir.c_str(), 0755);
}
}  // namespace

int main(int argc, char** argv)
{
    std::string zonePath = "./config/zoneConfig.yaml";
    std::string scenePath = "./config/sceneConfig.yaml";

    // 允许用命令行覆盖（两个位置参数，顺序：zone scene）
    if (argc > 1)
        zonePath = argv[1];
    if (argc > 2)
        scenePath = argv[2];

    // ---------------- 0. 信号基础设置（必须在创建任何线程之前）----------------
    // ★ 顺序为什么关键：线程会**继承创建时刻的信号掩码**。
    //   若在 asyncLogger.start()、SceneServer::start() 之后才阻塞这三个信号，
    //   工作线程（日志/网络/逻辑/DB）就会以"未阻塞"的掩码运行 —— 任何被投递到
    //   这些线程的 SIGTERM/SIGINT 都会按默认动作**直接杀死进程**，抢走主线程
    //   sigwait 的优雅退出流程。
    //   实测（旧顺序）：向某个工作线程发 SIGTERM → 进程 exit 143，日志里
    //   没有任何 "signal 15 received"（主线程根本没机会处理）。
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);

    if (pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0)
    {
        std::cerr << "pthread_sigmask failed: " << std::strerror(errno) << std::endl;
        return 1;
    }

    // SIGPIPE：向已关闭的连接写数据会触发它，而它的默认动作是**终止进程**。
    // 场景服的网络层没有用 MSG_NOSIGNAL（网关侧用了），所以在进程级显式忽略：
    // 写失败改由 errno=EPIPE 体现，交给各连接自己的错误处理路径。
    // 实测：旧代码下这个忽略是某个第三方库在 DB 连接时才顺手设置的（时机不受控），
    // 这里提前到启动最开始，把"偶然"变成"确定"。
    ::signal(SIGPIPE, SIG_IGN);

    // 自证：把生效的掩码读回来打印一次（避免"以为设了其实没设"）
    {
        sigset_t cur;
        pthread_sigmask(SIG_BLOCK, nullptr, &cur);   // 只读，不改
        std::cout << "signal mask applied: SIGINT=" << (sigismember(&cur, SIGINT) == 1)
                  << " SIGTERM=" << (sigismember(&cur, SIGTERM) == 1)
                  << " SIGUSR1=" << (sigismember(&cur, SIGUSR1) == 1)
                  << " SIGPIPE=ignored" << std::endl;
    }

    // ---------------- 1. 配置（区服共享 + 场景服特有）----------------
    std::string err;

    ZoneConfig zone;
    if (!ZoneConfig::Load(zonePath, zone, err))
    {
        std::cerr << "load zone config failed: " << err << std::endl;
        return 1;
    }

    SceneConfig cfg;
    if (!SceneConfig::Load(scenePath, cfg, err))
    {
        std::cerr << "load scene config failed: " << err << std::endl;
        return 1;
    }

    // 合并 + 派生 + 整体校验（net.zoneId、Redis 命名空间、logicThreadNum 等）
    if (!cfg.AttachZone(zone, err))
    {
        std::cerr << "attach zone config failed: " << err << std::endl;
        return 1;
    }

    // ---------------- 2. 日志（区服统一策略）----------------
    EnsureDir(cfg.zone.logDir);
    AsyncLogger asyncLogger(cfg.zone.logName, cfg.zone.logRollSize, cfg.zone.logDir);
    asyncLogger.start();
    Logger::set_AsyncLogger(&asyncLogger);
    Logger::set_GlobalLevel(ParseLogLevel(cfg.zone.logLevel));
    LOG_INFO << "logger ready, level=" << cfg.zone.logLevel << " dir=" << cfg.zone.logDir
             << " name=" << cfg.zone.logName;

    // 配置汇总一条：zoneId/sceneId 只在各自文件里定义一处，这里打印出来便于核对
    LOG_INFO << "config ready, zoneId=" << cfg.zone.zoneId << " sceneId=" << cfg.sceneId
             << " serviceId=" << (int)cfg.serviceId
             << " redisNs=" << cfg.zone.db.redis.zoneId << "/" << cfg.zone.db.redis.sceneId
             << " redisShards=" << cfg.zone.db.redis.ShardCount()
             << " gateways=" << cfg.zone.gateways.size();

    // ---------------- 3. 路由表自检 ----------------
    const std::vector<std::string> routeErrs = RouteTable::Validate();
    if (!routeErrs.empty())
    {
        for (const auto& e : routeErrs)
            LOG_ERROR << "route table invalid: " << e;
        std::cerr << "route table invalid (" << routeErrs.size() << " errors)" << std::endl;
        return 1;
    }
    LOG_INFO << "route table ok, routes=" << RouteTable::All().size();

    // ---------------- 4. 启动 ----------------
    SceneServer server(cfg);
    if (!server.start())
    {
        LOG_ERROR << "scene server start failed";
        return 1;
    }

    // ---------------- 5. 信号循环 ----------------
    // 信号集合与掩码已在 main 开头（创建任何线程之前）设置完成，见"0. 信号基础设置"。
    // 这里只做主线程的等待与分发：统一用 sigwait 接管，不注册 handler。
    LOG_INFO << "scene server running, send SIGUSR1 to reload lua, SIGTERM/SIGINT to stop";

    bool running = true;
    while (running)
    {
        int sig = 0;
        if (sigwait(&set, &sig) != 0)
            continue;

        switch (sig)
        {
            case SIGUSR1:
                // 目录即版本：发布时新建目录、内容不再修改，出问题可指回旧目录
                LOG_INFO << "SIGUSR1 received, reload lua from " << cfg.luaDir;
                server.reloadLua(cfg.luaDir);
                LOG_INFO << "stats: " << server.stats();
                break;

            case SIGINT:
            case SIGTERM:
                LOG_INFO << "signal " << sig << " received, stopping...";
                running = false;
                break;

            default:
                break;
        }
    }

    server.stop();
    LOG_INFO << "bye";

    // 停日志前先摘掉后端，避免后端线程已退出却仍被写入
    Logger::set_AsyncLogger(nullptr);
    asyncLogger.stop();
    return 0;
}
