// ============================================================================
// gatewayServer/main.cpp —— 网关进程入口
//
// 职责：
//   1. 读配置（gatewayServer/config/gatewayConfig.yaml，可用 argv[1] 覆盖路径）
//   2. 初始化日志（公共库 zone_common/log）
//   3. 主线程创建 baseLoop，构造 GatewayServer（**两个监听端口**）并 start()
//   4. baseLoop.loop() 进入事件循环
//
// 两个监听端口 = 两类数据：
//   publicConfig  —— 公网客户端（ClientFrame 协议）
//   privateConfig —— 区服内其它服务器（GateFrame 协议）
//   具体协议解析、鉴权、转发与回程路由在 GatewayDispatcher 内（见 gatewayDispatcher.h）。
//
// 明确不在本阶段（避免误读完成度）：
//   - 优雅停服 / 信号处理：留阶段 6（当前进程由 SIGTERM 直接终止）
//   - 登录服签发的正式 token / TLS：阶段 5 / 7（当前是预共享票据的"最小鉴权"）
//   - 后端心跳与主动健康检查：阶段 4 的剩余部分（当前靠 TCP 断开 + 握手重注册）
// ============================================================================
#include "gatewayConfig.h"

#include "common/currentThread.h"
#include "event/eventLoop.h"
#include "gatewayDispatcher.h"
#include "gatewayServer.h"
#include "inetAddress.h"
#include "log/asyncLogger.h"
#include "log/logger.h"
#include "tcpConnection.h"      // 连接回调里用 conn->name()/connected()，需要完整类型

#include <cstddef>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace
{
// 日志滚动阈值与目录：阶段 4 引入区服共享配置后，这里改为从 zoneConfig 读取
constexpr uint64_t kLogRollSize = 64 * 1024 * 1024;
constexpr const char* kLogDir = "./logs";
constexpr const char* kLogName = "gateway";   // 与场景服的 "scene" 区分，避免写同一文件

void EnsureDir(const std::string& dir)
{
    if (!dir.empty())
        ::mkdir(dir.c_str(), 0755);
}
}  // namespace

int main(int argc, char** argv)
{
    const std::string cfgPath = (argc > 1) ? argv[1] : "./config/gatewayConfig.yaml";

    // ---------------- 1. 配置 ----------------
    GatewayConfig cfg;
    std::string err;
    if (!GatewayConfig::Load(cfgPath, cfg, err))
    {
        std::fprintf(stderr, "load gateway config failed: %s\n", err.c_str());
        return 1;
    }

    // ---------------- 2. 日志 ----------------
    EnsureDir(kLogDir);
    AsyncLogger asyncLogger(kLogName, kLogRollSize, kLogDir);
    asyncLogger.start();
    Logger::set_AsyncLogger(&asyncLogger);

    LogLevel level = LogLevel::INFO;
    if (cfg.logLevel == "debug")      level = LogLevel::DEBUG;
    else if (cfg.logLevel == "info")  level = LogLevel::INFO;
    else if (cfg.logLevel == "warn")  level = LogLevel::WARNING;
    else if (cfg.logLevel == "error") level = LogLevel::ERROR;
    Logger::set_GlobalLevel(level);

    LOG_INFO << "gateway config ready, zoneId=" << cfg.zoneId << " gatewayId=" << cfg.gatewayId
             << " public=" << cfg.publicListenAddr << ":" << cfg.publicListenPort
             << " private=" << cfg.privateListenAddr << ":" << cfg.privateListenPort
             << " eventThreadNum=" << cfg.eventThreadNum;

    if (cfg.clientToken == 0)
        LOG_WARNING << "gateway.clientToken == 0：客户端鉴权校验已关闭（仅联调可用，勿用于生产）";

    // ---------------- 3. 事件循环与网关 ----------------
    // baseLoop 必须在本线程创建并 loop()（EventLoop 是线程亲和的）
    EventLoop baseLoop;

    // 真实签名是 (ip, port, ipv6)
    InetAddress publicAddr(cfg.publicListenAddr, cfg.publicListenPort, false);
    InetAddress privateAddr(cfg.privateListenAddr, cfg.privateListenPort, false);

    // 数据面配置：把"公网接入策略 + 后端白名单 + 有界容量"整体传下去
    DispatcherConfig dcfg;
    dcfg.selfServiceId      = ServerID::kGateway;
    dcfg.zoneId             = cfg.zoneId;
    dcfg.gatewayId          = cfg.gatewayId;
    dcfg.defaultDstService  = cfg.defaultServiceId;
    dcfg.allowedServices    = cfg.allowedServices;
    dcfg.clientToken        = cfg.clientToken;
    dcfg.maxClientSessions  = cfg.maxClientSessions;
    dcfg.maxPendingRoutes   = cfg.maxPendingRoutes;
    dcfg.routeTtlMs         = cfg.routeTtlMs;

    GatewayServer server(&baseLoop,
                         [](EventLoop*) { Current::setThreadName("gateway-io"); },
                         publicAddr,
                         privateAddr,
                         dcfg,
                         cfg.eventThreadNum);

    // 连接生命周期只在这里打日志（协议/鉴权/路由都在 GatewayDispatcher 内完成）。
    // c/b 前缀 + 角色名让"这条连接属于哪类数据"在日志里一眼可分。
    server.setConnectionCallback([](const TcpConnectionPtr& conn) {
        LOG_INFO << (conn->role() == connRole::Client ? "client connection " : "backend connection ")
                 << conn->name() << (conn->connected() ? " established" : " closed");
    });

    server.start();
    LOG_INFO << "gateway started, public(clients) on " << cfg.publicListenAddr << ":"
             << cfg.publicListenPort << ", private(servers) on " << cfg.privateListenAddr << ":"
             << cfg.privateListenPort;

    // ---------------- 4. 进入事件循环 ----------------
    // 阻塞执行；本阶段没有 quit() 触发点（信号与优雅停服属于阶段 6）
    baseLoop.loop();

    server.stop();
    LOG_INFO << "gateway stopped";

    Logger::set_AsyncLogger(nullptr);   // 先摘后端再停线程，避免日志线程退出后仍被写入
    asyncLogger.stop();
    return 0;
}
