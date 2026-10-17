#include "event/eventLoop.h"
#include "inetAddress.h"
#include "socket.h"
#include "tcpConnection.h"
#include "gatewayDispatcher.h"
#include "base/gatewayPlayer.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// TcpConnection 生命周期与收发
//
// 用**真实 loopback TCP 连接**（内核分配临时端口）造出"已连接的一对 fd"，
// 把服务端那一端包成 Socket 交给 TcpConnection，从而在没有真实客户端协议的情况下验证：
//   1) 建立时通知上层（connectionCallback 收到 connected()==true）
//   2) **关闭时也通知上层**（0.13 修复的行为：原实现只在建立时通知，
//      业务侧永远收不到"连接已关闭"）
//   3) 同线程 send() 能真的把字节写出去
//
// 为什么不用 socketpair(AF_UNIX)：TcpConnection 构造时会设置 TCP_NODELAY，
// 而该选项在 UNIX 域套接字上不合法（setsockopt 失败 → Socket::setTcpNoDelay
// 内部的 assert(ret == 0) 会直接 abort 进程）。用真实 TCP 连接更贴近生产路径。
// ---------------------------------------------------------------------------

namespace
{
struct TcpPair
{
    int serverFd = -1;   // accept 得到的：交给 TcpConnection
    int clientFd = -1;   // connect 得到的：测试侧读写

    static void SetNonBlock(int fd)
    {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    TcpPair()
    {
        const int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0)
            return;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;   // 让内核分配空闲端口

        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listenFd, 1) != 0)
        {
            ::close(listenFd);
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        {
            ::close(listenFd);
            return;
        }

        clientFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (clientFd < 0 ||
            ::connect(clientFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            if (clientFd >= 0) ::close(clientFd);
            clientFd = -1;
            ::close(listenFd);
            return;
        }

        serverFd = ::accept(listenFd, nullptr, nullptr);
        ::close(listenFd);

        if (serverFd >= 0)
            SetNonBlock(serverFd);   // 生产里由 accept4(SOCK_NONBLOCK) 保证
    }

    ~TcpPair()
    {
        if (serverFd >= 0) ::close(serverFd);
        if (clientFd >= 0) ::close(clientFd);
    }
};
}  // namespace

TEST(GatewayDispatcherSceneSubmissionCountsAndCancelsStaleQueue)
{
    EventLoop loop;
    TcpPair backendPair;
    TcpPair clientPair;
    CHECK(backendPair.serverFd >= 0 && clientPair.serverFd >= 0);
    InetAddress local;
    InetAddress peer;
    auto backend = std::make_shared<TcpConnection>(&loop, "backend-unique",
        Socket::fromFd(backendPair.serverFd), local, peer);
    auto client = std::make_shared<TcpConnection>(&loop, "client-unique",
        Socket::fromFd(clientPair.serverFd), local, peer);
    backendPair.serverFd = clientPair.serverFd = -1;
    backend->connectEstablelished();
    client->connectEstablelished();
    DispatcherConfig cfg;
    cfg.allowedServices = {ServerID::kScene};
    GatewayDispatcher dispatcher(cfg);
    dispatcher.OnBackendConnected(backend);
    BackendHandshake hs;
    hs.serviceId = ServerID::kScene;
    hs.zoneId = 1;
    hs.sceneId = 1;
    hs.sceneIncarnation = 1;
    hs.targetGatewayId = 1;
    Buffer handshake;
    EncodeFrame(handshake, MakeHeader(NetMessageType::kSceneMsg, SysModule::kSYS,
        SysMethod::kHandshake, 0, 0, ServerID::kScene, ServerID::kGateway), PackBackendHandshake(hs));
    dispatcher.OnBackendMessage(backend, &handshake);
    const auto sid = dispatcher.OnClientConnected(client, 1);
    const auto feed = [&](ClientKind kind, uint16_t module, uint16_t method,
                          uint64_t request, const std::string& body) {
        Buffer frame;
        ClientHeader header;
        header.kind = kind;
        header.module = module;
        header.method = method;
        header.requestId = request;
        EncodeClientFrame(frame, header, body);
        dispatcher.OnClientMessage(client, &frame, sid);
    };
    feed(ClientKind::kControl, kClientSysModule, ClientSysMethod::kAuth, 1, MakeAuthBody(678, 0));
    for (uint64_t request = 2; request < 5; ++request)
        feed(ClientKind::kRequest, 3, 2, request, "business");
    // 提交必须排队，不能在同线程立即 send 越过其它线程任务。
    CHECK_EQ(dispatcher.Snapshot().sceneOnlineFramesSent, (uint64_t)0);
    loop.queueInLoop([&] {
        const auto stats = dispatcher.Snapshot();
        CHECK_EQ(stats.sceneOnlineFramesSent, (uint64_t)1);
        CHECK_EQ(stats.sceneBusinessFramesSent, (uint64_t)3);
        CHECK_EQ(stats.sceneBindingDeclareSkipped, (uint64_t)2);
        CHECK_EQ(stats.backendFramesSent, (uint64_t)4);
        // 此请求尚未提交就关闭会话，不得发送或复活已声明状态。
        feed(ClientKind::kRequest, 3, 2, 5, "must-not-send");
        dispatcher.OnClientClosed(sid);
    });
    loop.runAfter(0.02, [&] { loop.quit(); });
    loop.loop();
    const auto stats = dispatcher.Snapshot();
    CHECK_EQ(stats.sceneBusinessFramesSent, (uint64_t)3);
    CHECK_EQ(stats.sceneOnlineFramesSent, (uint64_t)1);
    CHECK_EQ(stats.sceneOfflineFramesSent, (uint64_t)1);
    CHECK_EQ(stats.backendFramesSent, (uint64_t)5);
    CHECK_EQ(stats.pendingRoutes, (size_t)0);
    backend->connectDestroyed();
    client->connectDestroyed();
}

TEST(TcpConnectionLifecycleNotifiesUpAndDown)
{
    EventLoop loop;
    TcpPair pair;
    CHECK(pair.serverFd >= 0 && pair.clientFd >= 0);

    InetAddress local;
    InetAddress peer;

    auto conn = std::make_shared<TcpConnection>(&loop, "test#1",
                                                Socket::fromFd(pair.serverFd), local, peer);
    pair.serverFd = -1;   // 所有权已交给 TcpConnection

    int connectedCount = 0;
    int closedCount = 0;
    bool closedSawDisconnected = false;

    conn->setConnectionCallback([&](const TcpConnectionPtr& c) {
        if (c->connected())
        {
            ++connectedCount;
        }
        else
        {
            ++closedCount;
            closedSawDisconnected = true;
        }
    });

    // 建立：应通知一次 connected
    conn->connectEstablelished();
    CHECK_EQ(connectedCount, 1);
    CHECK_EQ(closedCount, 0);
    CHECK(conn->connected());

    // 同线程 send：字节应真的写到对端
    const std::string payload = "ping";
    conn->send(payload);

    char recvBuf[16] = {0};
    const ssize_t n = ::read(pair.clientFd, recvBuf, sizeof(recvBuf));
    CHECK_EQ(n, (ssize_t)payload.size());
    CHECK(std::string(recvBuf, n > 0 ? static_cast<size_t>(n) : 0) == payload);

    // 关闭：必须通知上层，且此时 connected() 已为 false
    conn->connectDestroyed();
    CHECK_EQ(closedCount, 1);
    CHECK(closedSawDisconnected);
    CHECK(!conn->connected());

    // 重复调用不应重复通知（幂等）
    conn->connectDestroyed();
    CHECK_EQ(closedCount, 1);
}

// 真实关闭路径（handleClose：对端 FIN / forceClose / shutdown 都会走到这里）
// 也必须通知业务侧，且与后续的销毁路径**不重复通知**。
TEST(TcpConnectionHandleCloseNotifiesBusinessOnce)
{
    EventLoop loop;
    TcpPair pair;
    CHECK(pair.serverFd >= 0 && pair.clientFd >= 0);

    InetAddress local;
    InetAddress peer;

    auto conn = std::make_shared<TcpConnection>(&loop, "test#2",
                                                Socket::fromFd(pair.serverFd), local, peer);
    pair.serverFd = -1;

    int connectedCount = 0;
    int closedCount = 0;
    int closeCallbackCount = 0;
    bool closedSawDisconnected = false;

    conn->setConnectionCallback([&](const TcpConnectionPtr& c) {
        if (c->connected())
            ++connectedCount;
        else
        {
            ++closedCount;
            closedSawDisconnected = true;
        }
    });
    conn->setCloseCallback([&](const TcpConnectionPtr&) { ++closeCallbackCount; });

    conn->connectEstablelished();
    CHECK_EQ(connectedCount, 1);

    // 同线程 forceClose() → runInLoop 就地执行 → forceCloseInLoop → handleClose
    conn->forceClose();
    CHECK_EQ(closedCount, 1);
    CHECK(closedSawDisconnected);
    CHECK_EQ(closeCallbackCount, 1);
    CHECK(!conn->connected());

    // 关闭后再走一次销毁路径：不应重复通知（幂等）
    conn->connectDestroyed();
    CHECK_EQ(closedCount, 1);
    CHECK_EQ(closeCallbackCount, 1);
}
