#ifndef CLEARMOON_GATEWAY_ACCEPTOR_H
#define CLEARMOON_GATEWAY_ACCEPTOR_H

#include "base/noncopy.h"
#include "connRole.h"
#include "socket.h"
#include "event/eventLoop.h"
#include "event/channel.h"

#include <functional>

class EventLoop;
class InetAddress;

class Acceptor : public noncopyable
{
public:
using newConnectionCallback = std::function<void(Socket, InetAddress, connRole)>;

    Acceptor(EventLoop* loop, const InetAddress& addr, connRole role);
    ~Acceptor();

    void setConnectionCallback(const newConnectionCallback& cb) { connCallback_ = cb; }
    //由GatewayServer调用
    void listen();

    void close();
private:
    //传给channel的回调
    void handleRead();

    EventLoop* loop_;
    Socket listenSock_;

    Channel acceptChannel_;

    newConnectionCallback connCallback_;
    connRole role_;
    bool listening_ = false;
};
#endif