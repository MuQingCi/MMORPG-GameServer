#ifndef CLEARMOON_NET_NETSERVER_H
#define CLEARMOON_NET_NETSERVER_H

#include "common/queue.h"
#include "common/msgBus.h"
#include "common/buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>

class NetServer
{
public:
    NetServer(const std::string& addr, uint16_t port, MsgBus& bus);

    ~NetServer();

    bool start();
    void stop();

private:
struct Conn
    {
        int fd = -1;                    //连接对应的Fd
        uint64_t session = 0;
        uint64_t gatewayID;             //网关ID
        uint64_t playerId;              //玩家id
        bool peer_closed = false;       // 对端已断,只允许回写并等待逻辑踢除

        bool writing = false; //是否开启写事件监听

        Buffer sendBuffer;  //待发送数据缓冲区
        Buffer readBuffer;  //读缓冲
    };
    
    void run();

    void enableWrite(Conn& conn, bool on);
    void parseFrames(Conn& conn);

    void acceptNewConn();
    void processBusMsg();
    void doWrite(Conn& conn);
    void handleRead(Conn& conn);
    void handleWrite(Conn& conn);
    void handleClose(Conn& conn);

    bool started_ = false;

    std::thread thread_;

    std::string addr_;
    size_t port_;

    int epFd_;
    int listenFd_;
    int weakupFd_;

    std::atomic<uint64_t> nextSession_;
    // std::map<uint64_t, uint32_t> playerWorker_; //playerId->workerId
    std::map<uint64_t,Conn> conns_;     //session->Conn
    std::map<uint64_t, uint64_t> session_;  //session->playerId
    MsgBus* mBus_ = nullptr;
};

#endif