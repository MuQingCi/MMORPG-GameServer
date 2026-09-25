#include "gatewayServer.h"

#include "connRole.h"
#include "event/eventLoop.h"
#include "log/logger.h"
#include "timer/timerId.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace
{
int64_t GwServerNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* RoleTag(connRole role)
{
    return role == connRole::Client ? "c" : "b";
}

const char* RoleName(connRole role)
{
    return role == connRole::Client ? "CLIENT" : "BACKEND";
}
}  // namespace

GatewayServer::GatewayServer(EventLoop* loop,
                             const ThreadPoolInitCallback& cb,
                             const InetAddress& clientListenAddr,
                             const InetAddress& backendListenAddr,
                             const DispatcherConfig& dispatcherCfg,
                             size_t eventThreadNum)
    : baseloop_(loop),
      eventThreadPool_(cb, "ClearMoon", eventThreadNum),
      cAcceptor_(loop, clientListenAddr, connRole::Client),
      bAcceptor_(loop, backendListenAddr, connRole::Backend),
      threadName_("ClearMoon"),
      dispatcher_(dispatcherCfg)
{
    // 两个 Acceptor 各自携带自己的角色：这是"区分两类连接"的第一层，
    // 角色随新连接一起传下去，连接对象据此选择协议与处理路径。
    cAcceptor_.setConnectionCallback(
        [this](Socket sock, InetAddress peerAddress, connRole role) {
            newConnection(std::move(sock), std::move(peerAddress), role);
        });
    bAcceptor_.setConnectionCallback(
        [this](Socket sock, InetAddress peerAddress, connRole role) {
            newConnection(std::move(sock), std::move(peerAddress), role);
        });
}

GatewayServer::~GatewayServer()
{
    stop();
}

void GatewayServer::start()
{
    if (started_)
        return;

    // 依次启动 io 线程池与两个监听器
    eventThreadPool_.start();
    cAcceptor_.listen();
    bAcceptor_.listen();

    // 回程路由 TTL 清理：后端不响应时，在途记录必须能回收（否则慢连接会把表撑满）
    sweepTimerId_ = baseloop_->runEvery(1.0, [this] { dispatcher_.SweepRoutes(GwServerNowMs()); });

    started_ = true;
}

// 注意：本函数假定在 baseLoop 所在线程调用（当前只由 main 在 loop() 返回后调用）。
// TODO(阶段 6)：跨线程停服需要重新设计顺序与同步，不能直接在此加锁了事。
void GatewayServer::stop()
{
    if (!started_)
        return;

    started_ = false;

    if (sweepTimerId_.valid())
    {
        baseloop_->cancel(sweepTimerId_);
        sweepTimerId_ = TimerId();
    }

    cAcceptor_.close();
    bAcceptor_.close();

    auto conns = connections_;
    for (auto& kv : conns)
        kv.second->forceClose();

    // 刻意不在主线程里清空 connections_：TcpConnection 必须在**自己的 IO 线程**里析构，
    // 由 removeConnectionInLoop / connectDestroyed 完成（channel/poller 都是线程亲和的）。

    LogStats();
}

void GatewayServer::LogStats() const
{
    const GatewayDispatcher::Stats st = dispatcher_.Snapshot();

    LOG_INFO << "gateway stats: clientSessions=" << st.clientSessions
             << " backendReady=" << st.backendReady
             << " backendPending=" << st.backendPending
             << " pendingRoutes=" << st.pendingRoutes
             << " | clientFrames recv=" << st.clientFramesRecv << " sent=" << st.clientFramesSent
             << " | backendFrames recv=" << st.backendFramesRecv << " sent=" << st.backendFramesSent
             << " | rejected=" << st.clientFramesRejected
             << " dropped=" << st.backendFramesDropped
             << " decodeErrors(c/b)=" << st.clientDecodeErrors << "/" << st.backendDecodeErrors
             << " protoMismatch=" << st.protocolMismatch
             << " kicked=" << st.kickedSessions;
}

/**
 * @brief 新连接回调（由两个 Acceptor 各自在自己的 baseLoop 上调用）
 *
 * 两类连接走**同一个**连接管理路径，但：
 *   1. 角色（Client/Backend）决定进会话表还是注册表；
 *   2. 角色决定消息回调走哪一套解帧（ClientFrame / GateFrame）；
 *   3. 连接名带 c/b 前缀，日志与排障时一眼能分辨。
 *
 * @param socket      已 accept 的（非阻塞）套接字
 * @param peerAddress 对端地址
 * @param role        由 Acceptor 携带：这条连接来自哪个监听端口
 */
void GatewayServer::newConnection(Socket socket, InetAddress peerAddress, connRole role)
{
    baseloop_->assertInLoopThread();

    if (role != connRole::Client && role != connRole::Backend)
    {
        LOG_ERROR << "newConnection with unknown role=" << static_cast<int>(role) << ", drop it";
        return;   // socket 在此析构关闭
    }

    EventLoop* ioLoop = eventThreadPool_.getNextLoop();
    // 连接名唯一：nextConnId_ 只在这里自增一次（历史缺陷：同一连接里被自增两次，
    // 既造成编号跳号，又让"会话->连接"索引与"连接名"索引对不上）
    const std::string name = threadName_ + "-" + RoleTag(role) + "#" + std::to_string(nextConnId_++);
    InetAddress local = socket.getLocalAddr();

    auto connPtr = std::make_shared<TcpConnection>(ioLoop, name, std::move(socket), local, peerAddress);
    connPtr->setRole(role);
    connections_[name] = connPtr;

    if (role == connRole::Client)
    {
        // 公网连接：先建会话（未鉴权）。会话 id 记在连接上，并被消息回调按值捕获
        // （会话在连接关闭时才销毁，捕获值不会悬垂）
        const uint64_t sessionId = dispatcher_.OnClientConnected(connPtr, GwServerNowMs());
        connPtr->setClientSessionId(sessionId);
        connPtr->setMessageCallback(
            [this, sessionId](const TcpConnectionPtr& c, Buffer* buf, Timestamp) {
                dispatcher_.OnClientMessage(c, buf, sessionId);
            });
    }
    else
    {
        // 区服内网连接：登记为未认证节点，等它发握手帧上报身份
        dispatcher_.OnBackendConnected(connPtr);
        connPtr->setMessageCallback([this](const TcpConnectionPtr& c, Buffer* buf, Timestamp) {
            dispatcher_.OnBackendMessage(c, buf);
        });
    }

    // 连接关闭时清掉对应的会话/注册记录。该回调在**连接的 IO 线程**上被调用，
    // 它只访问构造后不再变化的 baseloop_，并把真正的清理工作转投回 baseLoop（见 removeConnection）。
    // 依赖不变式：GatewayServer 的生命周期覆盖 baseLoop 的运行期（main 中为栈对象）。
    connPtr->setConnectionCallback([this](const TcpConnectionPtr& c) {
        if (!c->connected())
        {
            if (c->role() == connRole::Client)
                dispatcher_.OnClientClosed(c->clientSessionId());
            else
                dispatcher_.OnBackendClosed(c);
        }
        if (connectionCallback_)
            connectionCallback_(c);
    });
    connPtr->setCloseCallback([this](const TcpConnectionPtr& c) { removeConnection(c); });
    connPtr->setWriteCompleteCallback(writeCompleteCallback_);

    ioLoop->runInLoop([connPtr] { connPtr->connectEstablelished(); });

    LOG_INFO << "new " << RoleName(role) << " conn " << name
             << " peer=" << peerAddress.toIpPort() << " local=" << local.toIpPort();
}

void GatewayServer::removeConnection(const TcpConnectionPtr& conn)
{
    // 延后到 baseloop 本轮事件批处理结束后再移除/析构连接（防止 handleEvent 悬垂）
    // TODO(阶段 6 优雅停服)：若停服允许跨线程触发，这里要改为可延长生命周期的句柄，
    // 并把"停止接收 -> 排空在途任务 -> 关闭连接 -> join 线程"的顺序固化下来。
    baseloop_->queueInLoop([this, conn] { removeConnectionInLoop(conn); });
}

void GatewayServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
    baseloop_->assertInLoopThread();
    size_t n = connections_.erase(conn->name());
    if (n != 1)
        return;
    EventLoop* ioloop = conn->getLoop();
    ioloop->runInLoop([conn] { conn->connectDestroyed(); });
}
