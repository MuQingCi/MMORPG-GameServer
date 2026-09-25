#ifndef CLEARMOON_GATEWAY_DISPATCHER_H
#define CLEARMOON_GATEWAY_DISPATCHER_H

#include "clientSession.h"
#include "returnRoute.h"
#include "serviceRegistry.h"

#include "base/buffer.h"
#include "base/clientProto.h"
#include "common/callbacks.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 网关数据面配置（全部来自 gatewayConfig.yaml 的 gateway.* 段）
 */
struct DispatcherConfig
{
    uint8_t  selfServiceId      = 1;    // 本网关在服务间链路里的服务号（ServerID::kGateway）
    uint32_t zoneId             = 1;    // 所属区服：与后端握手上报的 zoneId 必须一致
    uint32_t gatewayId          = 1;
    uint8_t  defaultDstService  = 8;    // 客户端未显式绑定后端服务时的默认目标（ServerID::kScene_1）
    std::vector<uint8_t> allowedServices;  // 允许注册的后端服务号白名单（空 = 不允许任何后端注册）
    uint64_t clientToken        = 0;    // 客户端最小鉴权票据；0 = 不校验（仅联调，启动时告警）

    size_t   maxClientSessions  = 4096;
    size_t   maxPendingRoutes   = 10000;
    uint32_t routeTtlMs         = 15000;
    uint32_t maxDecodeErrors    = 32;
    size_t   maxReadBufferBytes = 1024 * 1024;
    uint32_t maxFramesPerSec    = 500;
};

/**
 * @brief 网关数据面：**把两类链路彻底分开处理**
 *
 * 两类链路（这与"两个 Acceptor"是同一个区分的两个层面：传输层端口 + 协议层帧）：
 *
 *   Client  链路（公网端口）
 *     帧格式 : base/clientProto.h 的 ClientFrame（20B 头、魔数 0xC1EB、有 kind/requestId）
 *     信任级别: **不可信**。playerId/zoneId 一律由网关在鉴权后绑定，客户端包头里的任何
 *              身份字段都不参与路由；未鉴权连接只能发控制帧。
 *     入站   : 鉴权 / 绑定后端服务 / 心跳 / 业务请求
 *     出站   : 响应（按内部 seq 反查回程路由）、推送（按 playerId 反查会话）、错误
 *
 *   Backend 链路（区服内网端口）
 *     帧格式 : base/proto.h 的 GateFrame（32B 头、魔数 0xC1EA、有 src/dst 服务号）
 *     信任级别: 需先握手认证（serviceId 白名单 + zoneId 一致），认证前收到的业务帧一律断开
 *     入站   : 握手（SYS/kHandshake）/ 响应 / 推送
 *     出站   : 业务请求（src=网关，dst=目标服务，seq 替换为网关内唯一的 internalSeq）
 *
 * 两层区分都fail-closed：
 *   1) 端口层：两个 Acceptor 各自只接一类连接（见 GatewayServer）；
 *   2) 协议层：魔数不同，混用时立即告警并断开，而不是当脏数据默默重同步（见 CheckProtocolMagic）。
 */
class GatewayDispatcher
{
public:
    struct Stats
    {
        uint64_t clientFramesRecv      = 0;
        uint64_t clientFramesSent      = 0;
        uint64_t backendFramesRecv     = 0;
        uint64_t backendFramesSent     = 0;
        uint64_t clientFramesRejected  = 0;   // 未鉴权/越权/无可用后端/超容量
        uint64_t backendFramesDropped  = 0;   // 未注册节点发来的帧 / 找不到回程路由
        uint64_t clientDecodeErrors    = 0;
        uint64_t backendDecodeErrors   = 0;
        uint64_t protocolMismatch      = 0;   // 端口上出现另一套协议的魔数
        uint64_t kickedSessions        = 0;

        size_t   clientSessions        = 0;
        size_t   backendReady          = 0;
        size_t   backendPending        = 0;
        size_t   pendingRoutes         = 0;
    };

    explicit GatewayDispatcher(const DispatcherConfig& cfg);

    // ---------------- 连接生命周期（baseLoop 线程调用）----------------
    // 新客户端连接：建会话（未鉴权）并返回 sessionId（连接会记住它，用于消息回调）
    uint64_t OnClientConnected(const TcpConnectionPtr& conn, int64_t nowMs);
    void     OnClientClosed(uint64_t sessionId);

    void     OnBackendConnected(const TcpConnectionPtr& conn);
    void     OnBackendClosed(const TcpConnectionPtr& conn);

    // ---------------- 数据面（连接所属 IO 线程调用）----------------
    void OnClientMessage(const TcpConnectionPtr& conn, Buffer* buf, uint64_t sessionId);
    void OnBackendMessage(const TcpConnectionPtr& conn, Buffer* buf);

    // ---------------- 周期维护（baseLoop 定时器）----------------
    size_t SweepRoutes(int64_t nowMs);

    Stats Snapshot() const;

private:
    void HandleClientFrame(const TcpConnectionPtr& conn, uint64_t sessionId,
                           const ClientHeader& h, const std::string& body);
    void HandleBackendFrame(const TcpConnectionPtr& conn, const Header& h, const std::string& body);

    // 出站：给客户端发一帧（响应/推送/错误）
    void SendToClient(const TcpConnectionPtr& conn, ClientKind kind, uint16_t module,
                      uint16_t method, uint64_t requestId, const std::string& body);

    // 拒绝客户端请求：回一个 kError 帧，并计数
    void RejectClient(const TcpConnectionPtr& conn, uint64_t requestId, uint16_t method,
                      const char* reason);

    // 每连接的流量守卫（decode 错误计数 + 帧速率）：越界则返回 false，调用方必须断开连接
    // 注意：速率是**按帧**计数的（见 AllowFrame），不是按"收包回调次数"——
    // 一次回调可能解出成千上万帧，按回调计数等于没拦（1MB 突发 ≈ 5 万个最小帧）。
    bool CheckReadBuffer(const TcpConnectionPtr& conn);
    bool AllowFrame(const TcpConnectionPtr& conn, int64_t nowMs);

    // 协议魔数交叉校验：客户端端口上收到服务间魔数（或反之）立刻断开
    bool CheckProtocolMagic(const TcpConnectionPtr& conn, Buffer* buf, bool clientLink);

    bool IsAllowedService(uint8_t serviceId) const;

    DispatcherConfig cfg_;

    ClientSessionTable sessions_;
    ServiceRegistry    services_;
    ReturnRouteTable   routes_;

    std::atomic<uint64_t> nextSeq_{1};

    std::atomic<uint64_t> clientFramesRecv_{0};
    std::atomic<uint64_t> clientFramesSent_{0};
    std::atomic<uint64_t> backendFramesRecv_{0};
    std::atomic<uint64_t> backendFramesSent_{0};
    std::atomic<uint64_t> clientFramesRejected_{0};
    std::atomic<uint64_t> backendFramesDropped_{0};
    std::atomic<uint64_t> clientDecodeErrors_{0};
    std::atomic<uint64_t> backendDecodeErrors_{0};
    std::atomic<uint64_t> protocolMismatch_{0};
    std::atomic<uint64_t> kickedSessions_{0};
};

#endif
