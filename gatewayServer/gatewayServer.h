#ifndef CLEARMOON_GATEWAY_SERVER_H
#define CLEARMOON_GATEWAY_SERVER_H

#include "common/callbacks.h"
#include "event/eventThreadPool.h"
#include "acceptor.h"
#include "inetAddress.h"
#include "socket.h"


#include <functional>
#include <cstddef>
#include <memory>
#include <unordered_map>

class EventThreadPool;
class EventLoop;

class GatewayServer
{
public:
using ThreadPoolInitCallback = std::function<void(EventLoop*)>;
    // eventThreadNum: IO 线程数（<= 0 时由线程池自行决定，仅测试/默认场景使用）
    //   —— 不把它写死在构造函数里，是因为配置项 gateway.eventThreadNum 必须真的生效，
    //      否则就是一栏"改了没用"的死配置。
    GatewayServer(EventLoop* loop,
                  const ThreadPoolInitCallback& cb,
                  const InetAddress& listenAddr,
                  size_t eventThreadNum = 0);
    ~GatewayServer();
    //设置回调
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb){ writeCompleteCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }

    void start();
    void stop();

private:
    void newConnection(Socket socket, InetAddress peerAddress);

    //第一个函数对TcpConnection对应的ioloop进行跨线程投递第二个函数
    void removeConnection(const TcpConnectionPtr& conn);
    //结果：在对应loop中对对应Channel调用disableAll()与remove();
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    EventLoop* baseloop_;
    EventLoopThreadPool eventThreadPool_;
    
    Acceptor acceptor_;

    std::string threadName_;
    bool started_;
    int nextConnId_;

    std::unordered_map<std::string, TcpConnectionPtr> connections_;

    ConnectionCallback connectionCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    MessageCallback messageCallback_;
};

#endif