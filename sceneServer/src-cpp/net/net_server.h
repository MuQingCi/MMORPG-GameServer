#ifndef CLEARMOON_NET_NETSERVER_H
#define CLEARMOON_NET_NETSERVER_H

#include "common/queue.h"
#include "common/msgBus.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <map>


class NetServer
{
public:
    NetServer(const std::string& addr, uint16_t port, MsgBus& bus);

    ~NetServer();

    bool start();
    void stop();

private:
    NetServer();
    
    void run();

    void acceptNewConn();
    void handleRead();
    void handleWrite();
    void handleClose();

    bool started_ = false;

    std::thread thread_;

    std::string addr_;
    size_t port_;

    size_t epFd_;
    size_t listenFd_;
    size_t weakupFd_;

    std::atomic<uint64_t> nextId_;

    struct Conn
    {
        uint64_t gatewayID;             //网关ID
        uint64_t playerId;              //玩家id
        bool peer_closed = false;       // 对端已断,只允许回写并等待逻辑踢除
        size_t fd = -1;                 //连接对应的Fd
        size_t id;
        std::string body;               //数据
    };
    std::map<uint64_t,Conn> conns_;     //session->Conn
    MsgBus* mBus_ = nullptr;
};

#endif