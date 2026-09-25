#ifndef CLEARMOON_NET_NETSERVER_H
#define CLEARMOON_NET_NETSERVER_H

#include "base/buffer.h"
#include "common/msg.h"
#include "common/msgBus.h"
#include "base/proto.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * @brief 网络线程配置
 *
 * 这里的每个上限都对应一类线上事故：
 *   - maxReadBuffer : 对端持续发而逻辑层消费不过来 -> 读缓冲无限增长 OOM
 *   - maxWriteBuffer: 慢客户端 + 推包量大 -> 写缓冲无限增长 OOM
 *   - maxDecodeErrors: 恶意/失配客户端 -> 无限重同步把网络线程 CPU 打满
 *   - maxFramesPerSec: 刷包攻击 -> 逻辑线程被单一连接饥饿
 */
struct NetConfig
{
    bool        listenEnable = false;         // 场景服通常不需要监听（反向连网关）
    std::string listenAddr   = "0.0.0.0";
    uint16_t    listenPort   = 12345;
    uint8_t     serviceId    = ServerID::kScene_1;  // 本服务 id（出站帧 srcServiceID）
    uint32_t    zoneId       = 1;             // 区服 id（握手时上报网关）
    uint32_t    sceneId      = 1;             // 场景 id（握手时上报网关）

    size_t   maxReadBufferBytes  = 1 * 1024 * 1024;
    size_t   maxWriteBufferBytes = 4 * 1024 * 1024;
    uint32_t maxFramesPerSec     = 1000;
    uint32_t maxDecodeErrors     = 64;
};

/**
 * @brief 需要反向连接的对端（网关集群）
 *
 * 设计文档 3.1.3 修正方向：场景服启动时**主动**连接网关集群（可连多个做冗余），
 * 而不是等网关拿连接池来连。理由：响应无法与请求匹配、回程地址无法表达、
 * 应用层重试对非幂等请求（释放技能）危险。
 */
struct PeerConfig
{
    uint8_t     serviceId = ServerID::kGateway;
    std::string addr = "127.0.0.1";
    uint16_t    port = 10000;
};

/**
 * @brief 场景服网络线程（单线程 epoll + ET）
 *
 * 职责边界（非常重要，越界就会产生数据竞争）：
 *   - 独占持有：fd、读/写缓冲、半包重组、会话->worker 亲和表；
 *   - 对外只通过 MsgBus 收发 Msg：其它线程**不允许**碰 Conn / fd / Buffer；
 *   - 出站消息的传输层字段（src/dst/链路类型/重试标记）由本层补齐，
 *     业务层只提供 Module/Method/seq/playerId/body。
 *
 * 两条投递路径：
 *   出站：逻辑线程 MsgBus::sendToNet(MSGTYPE_SEND) -> 唤醒 eventfd -> 本线程编码落帧
 *   入站：本线程 DecodeFrame -> MsgBus::sendToWorker(会话亲和 或 玩家 Actor 亲和)
 */
class NetServer
{
public:
    NetServer(const NetConfig& cfg, MsgBus& bus);
    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    bool start();
    void stop();

    // 反向连接：非阻塞 connect，完成后再向对应逻辑线程投 MSGTYPE_CONN_NEW
    bool connectTo(const PeerConfig& peer);

private:
    struct Conn
    {
        int      fd = -1;
        uint64_t session = 0;
        uint64_t playerId = 0;               // 最近一次在该连接上出现的玩家（仅用于日志/关闭通知）
        uint8_t  peerServiceID = ServerID::kServiceAny;
        uint8_t  linkType = NetMessageType::kSceneMsg;  // 出站帧的链路标识
        bool     outbound = false;           // 本端主动发起的连接
        bool     connecting = false;         // 非阻塞 connect 尚未完成
        bool     peerClosed = false;         // 对端已断，只允许把残余写完
        bool     writing = false;            // 是否已关注 EPOLLOUT

        uint32_t decodeErrors = 0;
        uint64_t rateWindowMs = 0;           // 帧速率统计窗口
        uint32_t framesInWindow = 0;

        Buffer sendBuffer;
        Buffer readBuffer;
    };

    void run();
    void wakeup();                                   // 唤醒 epoll（eventfd）
    void drainWakeup();

    void acceptNewConn();
    void onPeerReadable(Conn& conn);
    void onPeerWritable(Conn& conn);                 // 连接建立完成
    void onFrame(Conn& conn, const Header& head, const std::string& body);
    void handleHandshake(Conn& conn, const std::string& body);
    void processBusMsg();

    void sendTo(Conn& conn, const Msg& m);           // 编码成完整帧后追加并尝试立即写
    void doWrite(Conn& conn);                        // 唯一的写路径
    void enableWrite(Conn& conn, bool on);
    void handleClose(Conn& conn);                    // 会从 conns_ 中删除，调用方必须立即 return

    bool registerConn(Conn&& conn, uint32_t events);
    MsgBus::WorkerId workerForSession(uint64_t session, bool assignIfMissing, uint64_t playerId);

    MsgBus* m_bus_ = nullptr;
    NetConfig cfg_;

    std::atomic<bool> started_{false};
    std::thread thread_;

    int epFd_ = -1;
    int listenFd_ = -1;
    int wakeupFd_ = -1;

    std::atomic<uint64_t> nextSession_{1};

    // 以下成员只允许网络线程访问
    std::unordered_map<uint64_t, Conn> conns_;
    std::unordered_map<uint64_t, MsgBus::WorkerId> sessionWorker_;
};

#endif
