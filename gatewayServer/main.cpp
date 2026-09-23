// ============================================================================
// gatewayServer/main.cpp —— 网关进程入口
//
// 本阶段（阶段 0）只做"能启动、能在配置端口监听"：
//   1. 读配置（gatewayServer/config/gatewayConfig.yaml，可用 argv[1] 覆盖路径）
//   2. 初始化日志（公共库 zone_common/log）
//   3. 主线程创建 baseLoop，构造 GatewayServer 并 start()
//   4. baseLoop.loop() 进入事件循环
//
// 明确不在本阶段（避免误读完成度）：
//   - 优雅停服 / 信号处理：留阶段 6（当前进程由 SIGTERM 直接终止）
//   - 客户端协议解析：阶段 3 才定稿协议；这里收到数据只记录字节数并消费
//   - 后端连接、服务认证与注册：阶段 4
// ============================================================================
#include "gatewayConfig.h"

#include "common/currentThread.h"
#include "event/eventLoop.h"
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
    Logger::set_GlobalLevel(LogLevel::INFO);

    LOG_INFO << "gateway config ready, zoneId=" << cfg.zoneId << " gatewayId=" << cfg.gatewayId
             << " listen=" << cfg.listenAddr << ":" << cfg.listenPort
             << " eventThreadNum=" << cfg.eventThreadNum;

    // ---------------- 3. 事件循环与网关 ----------------
    // baseLoop 必须在本线程创建并 loop()（EventLoop 是线程亲和的）
    EventLoop baseLoop;

    // 真实签名是 (ip, port, ipv6)
    InetAddress listenAddr(cfg.listenAddr, cfg.listenPort, false);

    GatewayServer server(&baseLoop,
                         [](EventLoop*) { Current::setThreadName("gateway-io"); },
                         listenAddr,
                         cfg.eventThreadNum);

    // 最小回调：让"accept 与收包链路是通的"在日志里可见（协议解析在阶段 3）
    server.setConnectionCallback([](const TcpConnectionPtr& conn) {
        LOG_INFO << "connection " << conn->name()
                 << (conn->connected() ? " established" : " closed");
    });
    server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
        const size_t n = buf->readableBytes();
        LOG_INFO << "recv " << n << " bytes from " << conn->name();
        // 阶段 3 之前没有协议解析：必须消费掉数据，否则读缓冲会无界增长
        buf->retrieveAll();
    });

    server.start();
    LOG_INFO << "gateway started, listening on " << cfg.listenAddr << ":" << cfg.listenPort;

    // ---------------- 4. 进入事件循环 ----------------
    // 阻塞执行；本阶段没有 quit() 触发点（信号与优雅停服属于阶段 6）
    baseLoop.loop();

    server.stop();
    LOG_INFO << "gateway stopped";

    Logger::set_AsyncLogger(nullptr);   // 先摘后端再停线程，避免日志线程退出后仍被写入
    asyncLogger.stop();
    return 0;
}
