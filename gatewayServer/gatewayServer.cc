#include "gatewayServer.h"
#include "event/eventLoop.h"
#include "tcpConnection.h"

#include <cstddef>
#include <memory>
#include <string>
#include <sys/socket.h>

GatewayServer::GatewayServer(EventLoop* loop,
                             const ThreadPoolInitCallback& cb,
                             const InetAddress& listenAddr,
                             size_t eventThreadNum)
                    : baseloop_(loop), 
                      eventThreadPool_(cb, "ClearMoon", eventThreadNum),
                      acceptor_(loop, listenAddr),
                      threadName_("ClearMoon"),
                      started_(false),
                      nextConnId_(1)
{
    acceptor_.setConnectionCallback([this] (Socket sock, InetAddress peerAddress) { newConnection(std::move(sock), std::move(peerAddress)); });
}

GatewayServer::~GatewayServer()
{
    stop();
}

void GatewayServer::start()
{
    if (started_) return;

    //依次启动io线程池与Acceptor监听器
    eventThreadPool_.start();
    acceptor_.listen();

    started_ = true;
}

// 注意：本函数假定在 baseLoop 所在线程调用（当前只由 main 在 loop() 返回后调用）。
// TODO(阶段 6)：跨线程停服需要重新设计顺序与同步，不能直接在此加锁了事。
void GatewayServer::stop()
{
    if(!started_) return;

    started_ = false;

    acceptor_.close();
    auto conns = connections_;
    for(auto& kv : conns)
        kv.second->forceClose();

    // Do not clear `connections_` here to avoid destroying TcpConnection objects
    // in the main thread. Each TcpConnection will be destroyed in its IO loop
    // via `removeConnectionInLoop`/`connectDestroyed` to ensure channel/poller
    // operations happen in the correct thread.
}


/**
 * @brief 用于Acceptor处理新连接的回调函数
    1.先从io线程池中以轮询的方法取一个io线程
    2.初始化新连接的名称
    3.初始化连接对应的TcpConnectionPtr
    4.将其添加到对应的<string,TcpConnectionPtr>Map中
 * 
 * @param socket 
 * @param peerAddress 
 */
void GatewayServer::newConnection(Socket socket, InetAddress peerAddress)
{
    baseloop_->assertInLoopThread();

    EventLoop* ioLoop = eventThreadPool_.getNextLoop();


    std::string name = threadName_ + "#" + std::to_string(nextConnId_++);

    InetAddress local = socket.getLocalAddr();

    auto connPtr = std::make_shared<TcpConnection>(ioLoop, name, std::move(socket), local, peerAddress);
    connections_[name] = connPtr;

    //设置connPtr相关回调
    connPtr->setConnectionCallback(connectionCallback_);
    // 该回调在**连接的 IO 线程**上被调用（TcpConnection::handleClose）；
    // 它只访问构造后不再变化的 baseloop_，并把真正的清理工作转投回 baseLoop（见下）。
    // 依赖不变式：GatewayServer 的生命周期覆盖 baseLoop 的运行期（main 中为栈对象）。
    connPtr->setCloseCallback([this](const TcpConnectionPtr& c){ removeConnection(c); });
    connPtr->setMessageCallback(messageCallback_);
    connPtr->setWriteCompleteCallback(writeCompleteCallback_);

    ioLoop->runInLoop([connPtr]{ connPtr->connectEstablelished(); });
}

void GatewayServer::removeConnection(const TcpConnectionPtr& conn)
{
    // 延后到 baseloop 本轮事件批处理结束后再移除/析构连接（防止 handleEvent 悬垂）
    // TODO(阶段 6 优雅停服)：若停服允许跨线程触发，这里要改为可延长生命周期的句柄，
    // 并把"停止接收 -> 排空在途任务 -> 关闭连接 -> join 线程"的顺序固化下来。
    baseloop_->queueInLoop([this, conn]{ removeConnectionInLoop(conn); });
}

void GatewayServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
    baseloop_->assertInLoopThread();
    size_t n = connections_.erase(conn->name());
    if(n != 1) return;
    EventLoop* ioloop = conn->getLoop();
    ioloop->runInLoop([conn]{ conn->connectDestroyed(); });
}