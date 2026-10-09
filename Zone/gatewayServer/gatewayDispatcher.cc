#include "gatewayDispatcher.h"

#include "base/proto.h"
#include "base/gatewayPlayer.h"
#include "base/utils.h"
#include "log/logger.h"
#include "tcpConnection.h"   // TrafficGuard / role：每连接的流量守卫

#include <chrono>

namespace
{
int64_t GwNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 目标服务号 -> 服务间链路的链路标识（NetMessageType）。链路标识只用于日志/排障，
// 路由依据始终是 dstServiceID（见 base/proto.h 顶部说明）。
uint8_t LinkTypeOf(uint8_t serviceId)
{
    switch (serviceId)
    {
        case ServerID::kGateway: return NetMessageType::kGatewayMsg;
        case ServerID::kDB:      return NetMessageType::kDBMsg;
        case ServerID::kChat:    return NetMessageType::kChatMsg;
        case ServerID::kGlobal:  return NetMessageType::kGlobalMsg;
        default:                 return NetMessageType::kSceneMsg;
    }
}

bool HasMagic(const Buffer* buf, uint16_t magic)
{
    if (buf->readableBytes() < sizeof(uint16_t))
        return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf->peek());
    const uint16_t be = static_cast<uint16_t>((p[0] << 8) | p[1]);   // 帧头是多字节网络序
    return be == magic;
}

const char* ClientKindName(ClientKind k)
{
    switch (k)
    {
        case ClientKind::kRequest:  return "Request";
        case ClientKind::kResponse: return "Response";
        case ClientKind::kPush:     return "Push";
        case ClientKind::kError:    return "Error";
        case ClientKind::kControl:  return "Control";
        default:                    return "Unknown";
    }
}
}  // namespace

GatewayDispatcher::GatewayDispatcher(const DispatcherConfig& cfg)
    : cfg_(cfg)
{
    LOG_INFO << "dispatcher: zoneId=" << cfg_.zoneId << " gatewayId=" << cfg_.gatewayId
             << " defaultDst=" << ServerIdName(cfg_.defaultDstService)
             << " allowedServices=" << cfg_.allowedServices.size()
             << " clientToken=" << (cfg_.clientToken == 0 ? "DISABLED(dev only)" : "enabled")
             << " maxPendingRoutes=" << cfg_.maxPendingRoutes;
}

uint64_t GatewayDispatcher::OnClientConnected(const TcpConnectionPtr& conn, int64_t nowMs)
{
    const uint8_t dst = cfg_.defaultDstService;
    const uint64_t id = sessions_.Create(conn, dst, nowMs);

    LOG_INFO << "client session created: " << conn->name() << " session=" << id
             << " peer=" << conn->getPeerAddr().toIpPort()
             << " dst=" << ServerIdName(dst);

    if (sessions_.Size() > cfg_.maxClientSessions)
    {
        // 有界：公网入口的连接数必须封顶，否则一个 IP 就能把网关内存吃光
        LOG_WARNING << "too many client sessions (" << sessions_.Size()
                    << " > " << cfg_.maxClientSessions << "), close " << conn->name();
        conn->forceClose();
        return id;
    }
    return id;
}

void GatewayDispatcher::OnClientClosed(uint64_t sessionId)
{
    ClientSession s;
    const bool existed = sessions_.Get(sessionId, s);
    if(existed) NotifySceneBinding(s,false);
    sessions_.Erase(sessionId);

    // 会话销毁必须同时清掉它的在途回程路由：否则后端迟到的响应会命中一条"指向已死会话"的记录
    const size_t dropped = routes_.EraseBySession(sessionId);

    if (existed)
    {
        LOG_INFO << "client session closed: session=" << sessionId
                 << " playerId=" << s.playerId << " droppedRoutes=" << dropped;
    }
}

void GatewayDispatcher::NotifySceneBinding(const ClientSession& s, bool online)
{
    if(!s.authed || s.playerId==0 || s.dstService!=ServerID::kScene_1) return;
    ServiceEndpoint ep;
    if(!services_.GetByService(s.dstService,ep) || !ep.conn || ep.sceneId==0) return;
    ClientData d{s.playerId,s.sessionId,s.epoch,cfg_.gatewayId,ep.sceneId};
    Buffer out;
    EncodeFrame(out,MakeHeader(LinkTypeOf(ep.serviceId),GatewayPlayer::kModule,
        online ? GatewayPlayer::kOnline : GatewayPlayer::kOffline,0,s.playerId,
        cfg_.selfServiceId,ep.serviceId),GatewayPlayer::Pack(d));
    ep.conn->send(&out);
}

void GatewayDispatcher::OnBackendConnected(const TcpConnectionPtr& conn)
{
    services_.AddPending(conn->name(), conn);
    LOG_INFO << "backend conn accepted: " << conn->name()
             << " peer=" << conn->getPeerAddr().toIpPort() << " (waiting handshake)";
}

void GatewayDispatcher::OnBackendClosed(const TcpConnectionPtr& conn)
{
    ServiceEndpoint removed;
    if (!services_.RemoveByName(conn->name(), removed))
        return;

    LOG_INFO << "backend conn closed: " << conn->name()
             << " service=" << ServerIdName(removed.serviceId)
             << " zone=" << removed.zoneId << " scene=" << removed.sceneId;
}

size_t GatewayDispatcher::SweepRoutes(int64_t nowMs)
{
    const size_t n = routes_.Sweep(nowMs, cfg_.routeTtlMs);
    if (n > 0)
        LOG_WARNING << "sweep: dropped " << n << " expired pending routes (backend not responding?)";
    return n;
}

bool GatewayDispatcher::IsAllowedService(uint8_t serviceId) const
{
    for (uint8_t v : cfg_.allowedServices)
    {
        if (v == serviceId)
            return true;
    }
    return false;
}

// ===========================================================================
// 每连接的守卫：协议魔数交叉校验 + 解码错误计数 + 帧速率限制
// ===========================================================================
bool GatewayDispatcher::CheckProtocolMagic(const TcpConnectionPtr& conn, Buffer* buf, bool clientLink)
{
    // 两层区分的第 2 层（第 1 层是端口/Acceptor）：
    // 客户端端口只接受 0xC1EB，内网端口只接受 0xC1EA。混用时**立即断开并告警**——
    // 若把它当脏数据重同步，问题会表现成"偶发丢包"，极难定位。
    const uint16_t wrongMagic = clientLink ? kMagic : kClientMagic;
    if (!HasMagic(buf, wrongMagic))
        return true;

    protocolMismatch_.fetch_add(1, std::memory_order_relaxed);
    LOG_WARNING << (clientLink ? "client link got SERVICE frame (magic=0xC1EA)"
                               : "backend link got CLIENT frame (magic=0xC1EB)")
                << ", close " << conn->name()
                << " (wrong protocol on this port: 端口与协议必须匹配)";
    conn->forceClose();
    return false;
}

bool GatewayDispatcher::CheckReadBuffer(const TcpConnectionPtr& conn)
{
    // 读缓冲上限：对端持续发但拆不出整帧（或逻辑侧消费不过来）时必须断开而不是无限增长
    if (conn->readBufferBytes() > cfg_.maxReadBufferBytes)
    {
        LOG_WARNING << "read buffer overflow (" << conn->readBufferBytes() << "), close " << conn->name();
        conn->forceClose();
        return false;
    }
    return true;
}

bool GatewayDispatcher::AllowFrame(const TcpConnectionPtr& conn, int64_t nowMs)
{
    TcpConnection::TrafficGuard& g = conn->guard();

    // 1 秒滑动窗口；每帧调一次（调用方在**每次成功解出一帧**后调用）
    if (g.rateWindowMs == 0 || nowMs - g.rateWindowMs >= 1000)
    {
        g.rateWindowMs = nowMs;
        g.framesInWindow = 0;
    }
    if (++g.framesInWindow > cfg_.maxFramesPerSec)
    {
        LOG_WARNING << "frame rate exceeded (" << g.framesInWindow << "/s), close " << conn->name();
        conn->forceClose();
        return false;
    }
    return true;
}

void GatewayDispatcher::OnClientMessage(const TcpConnectionPtr& conn, Buffer* buf, uint64_t sessionId)
{
    const int64_t nowMs = GwNowMs();
    if (!CheckProtocolMagic(conn, buf, /*clientLink=*/true))
        return;
    if (!CheckReadBuffer(conn))
        return;

    for (;;)
    {
        ClientHeader h;
        std::string body;
        const DecodeStatus st = DecodeClientFrame(*buf, h, body);

        if (st == DecodeStatus::kNeedMore)
            return;   // 半包：等下次可读事件

        if (st == DecodeStatus::kError)
        {
            // 客户端方向是公网：累计到阈值就断开（可能是扫描器/攻击，也可能是版本不匹配）
            TcpConnection::TrafficGuard& g = conn->guard();
            if (++g.decodeErrors >= cfg_.maxDecodeErrors)
            {
                clientDecodeErrors_.fetch_add(g.decodeErrors, std::memory_order_relaxed);
                LOG_WARNING << "too many client decode errors, close " << conn->name();
                conn->forceClose();
                return;
            }
            continue;
        }

        clientFramesRecv_.fetch_add(1, std::memory_order_relaxed);
        if (!AllowFrame(conn, nowMs))
            return;
        HandleClientFrame(conn, sessionId, h, body);

        // 处理过程中可能已关闭连接（鉴权失败/越权/限速）
        if (!conn->connected())
            return;
    }
}

void GatewayDispatcher::OnBackendMessage(const TcpConnectionPtr& conn, Buffer* buf)
{
    const int64_t nowMs = GwNowMs();
    if (!CheckProtocolMagic(conn, buf, /*clientLink=*/false))
        return;
    if (!CheckReadBuffer(conn))
        return;

    for (;;)
    {
        Header h;
        std::string body;
        const DecodeStatus st = DecodeFrame(*buf, h, cfg_.selfServiceId, body);

        if (st == DecodeStatus::kNeedMore)
            return;

        if (st == DecodeStatus::kError)
        {
            TcpConnection::TrafficGuard& g = conn->guard();
            if (++g.decodeErrors >= cfg_.maxDecodeErrors)
            {
                backendDecodeErrors_.fetch_add(g.decodeErrors, std::memory_order_relaxed);
                LOG_WARNING << "too many backend decode errors, close " << conn->name();
                conn->forceClose();
                return;
            }
            continue;
        }

        backendFramesRecv_.fetch_add(1, std::memory_order_relaxed);
        if (!AllowFrame(conn, nowMs))
            return;
        // 与客户端链路对称的帧摘要（debug 级）：排障时能一眼看出"对端到底发了什么、
        // 解码出来的 src/dst/module/method 是不是预期的"
        LOG_DEBUG << "backend frame " << conn->name()
                  << " msgType=" << static_cast<int>(h.msgType)
                  << " src=" << ServerIdName(h.srcServiceID)
                  << " dst=" << ServerIdName(h.dstServiceID)
                  << " module=" << h.module << " method=" << h.method
                  << " seq=" << h.seq << " playerId=" << h.playerId
                  << " bodyLen=" << body.size();
        HandleBackendFrame(conn, h, body);

        if (!conn->connected())
            return;
    }
}

void GatewayDispatcher::SendToClient(const TcpConnectionPtr& conn, ClientKind kind, uint16_t module,
                                     uint16_t method, uint64_t requestId, const std::string& body,
                                     const std::string& roleName)
{
    if (!conn || !conn->connected())
        return;

    ClientHeader h;
    h.kind = kind;
    h.module = module;
    h.method = method;
    h.requestId = requestId;
    h.roleName = roleName;

    Buffer out;
    EncodeClientFrame(out, h, body);
    conn->send(&out);
    clientFramesSent_.fetch_add(1, std::memory_order_relaxed);
}

void GatewayDispatcher::RejectClient(const TcpConnectionPtr& conn, uint64_t requestId,
                                     uint16_t method, const char* reason)
{
    clientFramesRejected_.fetch_add(1, std::memory_order_relaxed);
    LOG_WARNING << "reject client request on " << conn->name() << ": " << reason
                << " method=" << method << " requestId=" << requestId;

    // 明确回一个 kError：让客户端立刻知道失败，而不是等到超时
    SendToClient(conn, ClientKind::kError, kClientSysModule, method, requestId,
                 std::string(reason != nullptr ? reason : "rejected"));
}

// ===========================================================================
// 客户端方向：控制帧（鉴权/绑定/心跳）+ 业务请求
// ===========================================================================
void GatewayDispatcher::HandleClientFrame(const TcpConnectionPtr& conn, uint64_t sessionId,
                                          const ClientHeader& h, const std::string& body)
{
    const int64_t nowMs = GwNowMs();

    ClientSession s;
    if (!sessions_.Get(sessionId, s))
        return;   // 会话已被销毁（连接即将关闭）

    LOG_DEBUG << "client frame " << conn->name() << " kind=" << ClientKindName(h.kind)
              << " module=" << h.module << " method=" << h.method << " requestId=" << h.requestId
              << " bodyLen=" << body.size();

    // ---------------- 控制通道（模块号 1）----------------
    if (h.kind == ClientKind::kControl || h.module == kClientSysModule)
    {
        if (h.kind != ClientKind::kControl || h.module != kClientSysModule)
        {
            RejectClient(conn, h.requestId, h.method, "control kind and module mismatch");
            return;
        }
        switch (h.method)
        {
            case ClientSysMethod::kAuth:
            {
                uint64_t playerId = 0;
                uint64_t ticket = 0;
                if (!ParseAuthBody(body, playerId, ticket) || playerId == 0)
                {
                    RejectClient(conn, h.requestId, ClientSysMethod::kAuthFail, "bad auth body");
                    return;
                }

                if (cfg_.clientToken != 0 && ticket != cfg_.clientToken)
                {
                    RejectClient(conn, h.requestId, ClientSysMethod::kAuthFail, "bad ticket");
                    // 鉴权失败必须断开：否则公网侧可以无限试票据
                    conn->shutdown();
                    return;
                }

                uint64_t kicked = 0;
                const uint8_t dst = (s.dstService != 0) ? s.dstService : cfg_.defaultDstService;
                if (!sessions_.Bind(sessionId, playerId, dst, nowMs, kicked))
                    return;   // 会话已消失

                if (kicked != 0)
                {
                    // 顶号：旧会话立刻失效，其连接关闭、在途回程路由作废
                    kickedSessions_.fetch_add(1, std::memory_order_relaxed);
                    routes_.EraseBySession(kicked);
                    ClientSession old;
                    if (sessions_.Get(kicked, old) && old.conn)
                        old.conn->forceClose();
                    sessions_.Erase(kicked);
                    LOG_WARNING << "session " << kicked << " kicked by new login, player=" << playerId;
                }

                ClientSession cur;
                if (!sessions_.Get(sessionId, cur))
                    return;

                NotifySceneBinding(cur,true);

                SendToClient(conn, ClientKind::kControl, kClientSysModule, ClientSysMethod::kAuthAck,
                             h.requestId,
                             MakeAuthAckBody(playerId, sessionId, cur.epoch, cfg_.zoneId, cur.dstService));
                LOG_INFO << "client authed: " << conn->name() << " player=" << playerId
                         << " session=" << sessionId << " epoch=" << cur.epoch
                         << " dst=" << ServerIdName(cur.dstService);
                return;
            }

            case ClientSysMethod::kBindService:
            {
                if (!s.authed)
                {
                    RejectClient(conn, h.requestId, ClientSysMethod::kBindService, "not authenticated");
                    return;
                }

                uint8_t serviceId = 0;
                if (!ParseServiceBody(body, serviceId) || !IsAllowedService(serviceId))
                {
                    RejectClient(conn, h.requestId, ClientSysMethod::kBindService, "service not allowed");
                    return;
                }

                if(!sessions_.SetDstService(sessionId, serviceId)) {
                    RejectClient(conn,h.requestId,h.method,"cannot update service binding");
                    return;
                }
                if(serviceId!=s.dstService) NotifySceneBinding(s,false);
                ClientSession current;
                if(sessions_.Get(sessionId,current)) NotifySceneBinding(current,true);
                SendToClient(conn, ClientKind::kControl, kClientSysModule, ClientSysMethod::kBindServiceAck,
                             h.requestId, MakeServiceBody(serviceId));
                LOG_INFO << "client " << sessionId << " bind service -> " << ServerIdName(serviceId);
                return;
            }

            case ClientSysMethod::kPing:
                SendToClient(conn, ClientKind::kControl, kClientSysModule, ClientSysMethod::kPong,
                             h.requestId, "");
                return;

            default:
                RejectClient(conn, h.requestId, h.method, "unknown control method");
                return;
        }
    }

    // ---------------- 业务请求 ----------------
    // 只有 Request 允许从客户端方向进入：客户端不能自己造 Response/Push/Error
    if (h.kind != ClientKind::kRequest)
    {
        RejectClient(conn, h.requestId, h.method, "only Request kind allowed from client");
        return;
    }

    if (h.requestId == 0)
    {
        RejectClient(conn, h.requestId, h.method, "requestId must be nonzero");
        return;
    }

    // 未鉴权连接不得访问业务路由。注意 playerId **不取客户端包头/包体**，
    // 只取会话上绑定的值 —— 这就是"伪造 playerId 被拒绝"的实现点。
    if (!s.authed)
    {
        RejectClient(conn, h.requestId, h.method, "not authenticated");
        return;
    }

    // 客户端角色名不是可信身份，仅聊天消息展示用；不将其扩散到场景服或全局服。
    if (h.module == ServiceModule::kChat && h.roleName.empty())
    {
        RejectClient(conn, h.requestId, h.method, "chat role name required");
        return;
    }

    // 仅允许预定义的聊天/全局模块独立路由；普通业务仍使用会话绑定的目标服务。
    // 不信任客户端提供服务号，且不能把未知模块随意送往其它后端。
    const uint8_t dst = h.module == ServiceModule::kChat ? static_cast<uint8_t>(ServerID::kChat)
                        : h.module == ServiceModule::kGlobal ? static_cast<uint8_t>(ServerID::kGlobal)
                        : ((s.dstService != 0) ? s.dstService : cfg_.defaultDstService);
    if (!IsAllowedService(dst))
    {
        RejectClient(conn, h.requestId, h.method, "service not allowed");
        return;
    }

    ServiceEndpoint ep;
    if (!services_.GetByService(dst, ep) || !ep.conn)
    {
        // 后端未注册（或刚断开）：明确回错误，不静默丢弃
        RejectClient(conn, h.requestId, h.method, "backend service unavailable");
        return;
    }

    // 网关内唯一的 internalSeq：两个客户端用**相同 requestId** 也互不干扰
    const uint64_t internalSeq = nextSeq_.fetch_add(1, std::memory_order_relaxed);

    ReturnRoute r;
    r.internalSeq = internalSeq;
    r.clientSessionId = sessionId;
    r.clientRequestId = h.requestId;
    r.backendServiceId = dst;
    r.playerId = s.playerId;
    r.module = h.module;
    r.method = h.method;
    r.sessionEpoch = s.epoch;
    r.createdAtMs = nowMs;

    if (!routes_.Add(r, cfg_.maxPendingRoutes))
    {
        RejectClient(conn, h.requestId, h.method, "too many pending requests");
        return;
    }

    // 转发：src=网关，dst=目标服务，playerId 用**会话绑定的**玩家，seq 换成 internalSeq
    Buffer out;
    std::string backendBody = body;
    if(dst == ServerID::kScene_1) {
        ClientData d{s.playerId,s.sessionId,s.epoch,cfg_.gatewayId,ep.sceneId};
        if(!GatewayPlayer::Valid(d) || h.module == GatewayPlayer::kModule) {
            routes_.Take(internalSeq,r);
            RejectClient(conn,h.requestId,h.method,"invalid scene binding or reserved gateway route");
            return;
        }
        // 每个请求前幂等声明当前绑定；后端重新连接后也能重新建立路由。
        EncodeFrame(out,MakeHeader(LinkTypeOf(dst),GatewayPlayer::kModule,GatewayPlayer::kOnline,
            0,s.playerId,cfg_.selfServiceId,dst),GatewayPlayer::Pack(d));
        backendBody = GatewayPlayer::Pack(d,body);
    }
    if (dst == ServerID::kChat && h.module == ServiceModule::kChat &&
        (h.method == ServiceMethod::kPrivateChat || h.method == ServiceMethod::kZoneShout))
    {
        // Chat 请求的 body 原为 targetId(u64 BE)+text；仅在网关可信边界
        // 注入客户端角色名，聊天服务不需要也无法解析公网 ClientFrame。
        if (h.method == ServiceMethod::kPrivateChat && body.size() < 8)
        {
            routes_.Take(internalSeq, r);
            RejectClient(conn, h.requestId, h.method, "invalid chat message");
            return;
        }
        const size_t prefix = h.method == ServiceMethod::kPrivateChat ? 8 : 0;
        backendBody = body.substr(0, prefix) + h.roleName +
                      std::string(kRoleNameSize - h.roleName.size(), '\0') + body.substr(prefix);
    }
    EncodeFrame(out,
                MakeHeader(LinkTypeOf(dst), h.module, h.method, internalSeq, s.playerId,
                           cfg_.selfServiceId, dst),
                 backendBody);
    ep.conn->send(&out);
    backendFramesSent_.fetch_add(1, std::memory_order_relaxed);

    LOG_INFO << "relay c->b: session=" << sessionId << " player=" << s.playerId
             << " dst=" << ServerIdName(dst) << " module=" << h.module << " method=" << h.method
             << " requestId=" << h.requestId << " -> seq=" << internalSeq
             << " bodyLen=" << body.size();
}

// ===========================================================================
// 后端方向：握手认证 + 响应/推送回程
// ===========================================================================
void GatewayDispatcher::HandleBackendFrame(const TcpConnectionPtr& conn, const Header& h,
                                           const std::string& body)
{
    const int64_t nowMs = GwNowMs();

    // ---------------- 握手（唯一的注册入口）----------------
    if (h.module == SysModule::kSYS && h.method == SysMethod::kHandshake)
    {
        BackendHandshake hs;
        if (!UnpackBackendHandshake(body, hs))
        {
            LOG_WARNING << "bad handshake body from " << conn->name();
            backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            conn->forceClose();
            return;
        }

        if (h.srcServiceID != hs.serviceId || h.seq != 0 || h.playerId != 0 || h.retrFlag != 0)
        {
            LOG_WARNING << "invalid backend handshake header from " << conn->name();
            conn->forceClose();
            return;
        }

        // 两条硬校验：白名单 + 区服一致。二者都不通过就断开，
        // 而不是"注册失败但连接留着"（否则后面每个业务帧都要重复判断）。
        if (!IsAllowedService(hs.serviceId))
        {
            LOG_WARNING << "reject backend " << conn->name() << ": service="
                        << ServerIdName(hs.serviceId) << " not in allowedServices";
            backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            conn->forceClose();
            return;
        }
        if (hs.zoneId != cfg_.zoneId)
        {
            LOG_WARNING << "reject backend " << conn->name() << ": zone=" << hs.zoneId
                        << " != gateway zone=" << cfg_.zoneId << " (跨区服连接被拒绝)";
            backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            conn->forceClose();
            return;
        }

        std::string oldConn;
        if (!services_.Bind(conn->name(), hs.serviceId, hs.zoneId, hs.sceneId, nowMs, &oldConn))
        {
            LOG_WARNING << "bind backend failed: " << conn->name()
                        << " (conn not accepted by this gateway?)";
            conn->forceClose();
            return;
        }

        if (!oldConn.empty())
        {
            // 同号节点重连：旧连接必须关闭，否则会存在两个"场景服"
            ServiceEndpoint old;
            if (services_.RemoveByName(oldConn, old))
            {
                LOG_WARNING << "backend " << ServerIdName(hs.serviceId) << " re-registered by "
                            << conn->name() << ", close stale conn " << oldConn;
                if (old.conn)
                    old.conn->forceClose();
            }
        }

        LOG_INFO << "backend registered: " << ServerIdName(hs.serviceId) << " conn=" << conn->name()
                 << " zone=" << hs.zoneId << " scene=" << hs.sceneId;
        return;
    }

    // ---------------- 业务帧：必须是已认证节点 ----------------
    ServiceEndpoint self;
    if (!services_.GetByName(conn->name(), self) || self.serviceId == 0)
    {
        // 阶段 4 要求：未认证连接不得发送业务消息（认证前唯一允许的是握手帧）
        backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
        LOG_WARNING << "business frame from UNREGISTERED backend conn=" << conn->name()
                    << " (module=" << h.module << " method=" << h.method << "), close it";
        conn->forceClose();
        return;
    }

    if (h.srcServiceID != self.serviceId || h.dstServiceID != cfg_.selfServiceId || h.retrFlag != 0)
    {
        LOG_WARNING << "backend header identity/retry mismatch from " << conn->name();
        conn->forceClose();
        return;
    }

    // 回程路径 1：按 internalSeq 反查（正常响应）。先核实来源和请求上下文，
    // 不允许其它服务或错误的 playerId/module/method 抢走路由。
    ReturnRoute r;
    if (h.seq != 0 && routes_.Get(h.seq, r))
    {
        if (r.backendServiceId != self.serviceId || r.playerId != h.playerId ||
            r.module != h.module || r.method != h.method)
        {
            LOG_WARNING << "backend response context mismatch: seq=" << h.seq;
            conn->forceClose();
            return;
        }
        if (!routes_.Take(h.seq, r))
            return;
        ClientSession s;
        if (sessions_.Get(r.clientSessionId, s) && s.authed && s.epoch == r.sessionEpoch && s.conn)
        {
            // requestId 还原成客户端自己的号；playerId 不回传给客户端（客户端无权知道）
            SendToClient(s.conn, ClientKind::kResponse, h.module, h.method, r.clientRequestId, body);
            LOG_INFO << "relay b->c: session=" << r.clientSessionId << " from="
                     << ServerIdName(self.serviceId) << " module=" << h.module
                     << " method=" << h.method << " seq=" << h.seq
                     << " -> requestId=" << r.clientRequestId << " bodyLen=" << body.size();
        }
        else
        {
            // 会话已销毁 / epoch 已过期（顶号、重连）：丢弃，不投给"现在占着这个 playerId 的人"
            backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARNING << "drop stale response: session=" << r.clientSessionId
                        << " epoch=" << r.sessionEpoch << " seq=" << h.seq;
        }
        return;
    }

    // 回程路径 2：seq==0 才是推送；非零未知 seq 是重复/超时响应，不得投给当前玩家。
    if (h.seq == 0 && h.module == ServiceModule::kChat &&
        h.method == ServiceMethod::kZoneShout && self.serviceId == ServerID::kChat &&
        h.playerId == 0)
    {
        if (body.size() < 8 + kRoleNameSize + 1 || body.size() > 8 + kRoleNameSize + 1024)
        {
            backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        size_t n = 0;
        while (n < kRoleNameSize && body[8 + n] != '\0') ++n;
        bool padded = n != 0;
        for (size_t i = n; i < kRoleNameSize; ++i)
            if (body[8 + i] != '\0') padded = false;
        const std::string name = body.substr(8, n);
        if (!padded || !ValidRoleName(name))
        {
            backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::string payload = body.substr(0, 8) + body.substr(8 + kRoleNameSize);
        for (const ClientSession& s : sessions_.AuthedSessions())
        {
            ClientSession cur;
            if (sessions_.Get(s.sessionId, cur) && cur.authed && cur.epoch == s.epoch &&
                cur.conn == s.conn)
                SendToClient(s.conn, ClientKind::kPush, h.module, h.method, 0, payload, name);
        }
        return;
    }
    if (h.seq == 0 && h.playerId != 0)
    {
        // 聊天推送只能来自聊天服务，不能让其它后端伪造聊天内容。
        if ((h.module == ServiceModule::kChat && self.serviceId != ServerID::kChat) ||
            (h.module == ServiceModule::kGlobal && self.serviceId != ServerID::kGlobal))
        {
            conn->forceClose();
            return;
        }
        ClientSession s;
        if (sessions_.FindByPlayer(h.playerId, s) && s.conn)
        {
            if (h.module == ServiceModule::kChat && h.method == ServiceMethod::kPrivateChat)
            {
                if (body.size() < 8 + kRoleNameSize)
                {
                    backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                size_t nameLen = 0;
                while (nameLen < kRoleNameSize && body[8 + nameLen] != '\0') ++nameLen;
                const std::string name = body.substr(8, nameLen);
                bool padded = true;
                for (size_t i = nameLen; i < kRoleNameSize; ++i)
                    if (body[8 + i] != '\0') padded = false;
                if (!padded || !ValidRoleName(name))
                {
                    backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                SendToClient(s.conn, ClientKind::kPush, h.module, h.method, 0,
                             body.substr(0, 8) + body.substr(8 + kRoleNameSize), name);
            }
            else
                SendToClient(s.conn, ClientKind::kPush, h.module, h.method, 0, body);
            LOG_INFO << "push b->c: player=" << h.playerId << " session=" << s.sessionId
                     << " from=" << ServerIdName(self.serviceId) << " module=" << h.module
                     << " method=" << h.method << " bodyLen=" << body.size();
        }
        else
        {
            backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARNING << "push for offline player=" << h.playerId << " dropped (from "
                        << ServerIdName(self.serviceId) << ")";
        }
        return;
    }

    backendFramesDropped_.fetch_add(1, std::memory_order_relaxed);
    LOG_WARNING << "unroutable backend frame: conn=" << conn->name()
                << " module=" << h.module << " method=" << h.method << " seq=" << h.seq
                << " playerId=" << h.playerId;
}

GatewayDispatcher::Stats GatewayDispatcher::Snapshot() const
{
    Stats st;
    st.clientFramesRecv     = clientFramesRecv_.load(std::memory_order_relaxed);
    st.clientFramesSent     = clientFramesSent_.load(std::memory_order_relaxed);
    st.backendFramesRecv    = backendFramesRecv_.load(std::memory_order_relaxed);
    st.backendFramesSent    = backendFramesSent_.load(std::memory_order_relaxed);
    st.clientFramesRejected = clientFramesRejected_.load(std::memory_order_relaxed);
    st.backendFramesDropped = backendFramesDropped_.load(std::memory_order_relaxed);
    st.clientDecodeErrors   = clientDecodeErrors_.load(std::memory_order_relaxed);
    st.backendDecodeErrors  = backendDecodeErrors_.load(std::memory_order_relaxed);
    st.protocolMismatch     = protocolMismatch_.load(std::memory_order_relaxed);
    st.kickedSessions       = kickedSessions_.load(std::memory_order_relaxed);

    st.clientSessions = sessions_.Size();
    st.backendReady   = services_.ReadyCount();
    const size_t all = services_.Size();
    st.backendPending = all > st.backendReady ? all - st.backendReady : 0;
    st.pendingRoutes  = routes_.Size();
    return st;
}
