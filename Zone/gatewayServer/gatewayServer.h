#ifndef CLEARMOON_GATEWAY_SERVER_H
#define CLEARMOON_GATEWAY_SERVER_H

#include "gatewayDispatcher.h"

#include "acceptor.h"
#include "common/callbacks.h"
#include "event/eventThreadPool.h"
#include "inetAddress.h"
#include "socket.h"
#include "tcpConnection.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class EventThreadPool;
class EventLoop;

/**
 * @brief 网关：两个监听端口 + 两个连接角色 + 数据面分发
 *
 * 结构（"区分两类连接"在**三层**上落地，任意一层都能独立挡住串流）：
 *
 *   端口/接受层   cAcceptor_（publicConfig，role=Client）
 *                 bAcceptor_（privateConfig，role=Backend）
 *        ↓
 *   连接/协议层   TcpConnection::role() 决定走 ClientFrame 还是 GateFrame 解码；
 *                 GatewayDispatcher::CheckProtocolMagic 再按魔数交叉校验一次
 *        ↓
 *   业务/路由层   Client 链路：鉴权 -> 会话 -> 业务请求（转发给后端）
 *                 Backend 链路：握手注册 -> 响应（按回程路由发回客户端）/ 推送（按 playerId）
 *
 * 连接所有权与生命周期：
 *   - `connections_`（name -> conn）是**唯一**的全局连接索引，只在 baseLoop 线程维护；
 *   - 客户端会话（sessionId -> conn）由 GatewayDispatcher 持有，本类**不再维护第二份**，
 *     避免"两个索引不一致"这类经典缺陷。
 */
class GatewayServer
{
public:
    using ThreadPoolInitCallback = std::function<void(EventLoop*)>;

    // eventThreadNum: IO 线程数（0 时由线程池自行决定，仅测试/默认场景使用）
    GatewayServer(EventLoop* loop,
                  const ThreadPoolInitCallback& cb,
                  const InetAddress& clientListenAddr,
                  const InetAddress& backendListenAddr,
                  const DispatcherConfig& dispatcherCfg,
                  size_t eventThreadNum = 0);
    ~GatewayServer();

    // 设置回调（仅用于观测/日志；会话与路由逻辑在 GatewayDispatcher 内）
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

    void start();
    void stop();

    // 输出两类链路的统计（停服时打一行，e2e 测试也据此断言）
    void LogStats() const;

private:
    void newConnection(Socket socket, InetAddress peerAddress, connRole role);

    // 第一个函数对 TcpConnection 对应的 ioloop 进行跨线程投递第二个函数
    void removeConnection(const TcpConnectionPtr& conn);
    // 结果：在对应 loop 中对对应 Channel 调用 disableAll() 与 remove();
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    EventLoop* baseloop_;
    EventLoopThreadPool eventThreadPool_;

    Acceptor cAcceptor_;
    Acceptor bAcceptor_;

    std::string threadName_;
    bool started_ = false;
    uint64_t nextConnId_ = 1;

    // 回程路由 TTL 清理（baseLoop 定时器）
    TimerId sweepTimerId_;

    std::unordered_map<std::string, TcpConnectionPtr> connections_;

    // 数据面：两类链路的协议解析、鉴权、转发与回程路由
    GatewayDispatcher dispatcher_;

    ConnectionCallback connectionCallback_;
    WriteCompleteCallback writeCompleteCallback_;
};

#endif
