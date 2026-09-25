#include "net/net_server.h"

#include "common/hash.h"
#include "base/proto.h"
#include "base/utils.h"
#include "log/logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace
{
// epoll 事件 data.u64 的两个保留标记（合法 session 从 1 开始自增，不会撞上）
constexpr uint64_t kListenTag = ~0ULL;
constexpr uint64_t kWakeupTag = ~0ULL - 1;

// 握手体的打包/解析已收敛到公共协议层（base/proto.h 的 PackBackendHandshake /
// UnpackBackendHandshake）：网关侧要解同一份字节，两份实现必然漂移。

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
}  // namespace

NetServer::NetServer(const NetConfig& cfg, MsgBus& bus)
    : m_bus_(&bus), cfg_(cfg)
{
}

NetServer::~NetServer()
{
    stop();
}

bool NetServer::start()
{
    if (started_.load(std::memory_order_acquire))
        return true;

    epFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epFd_ < 0)
    {
        LOG_ERROR << "epoll_create1 failed: " << std::strerror(errno);
        return false;
    }

    wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd_ < 0)
    {
        LOG_ERROR << "eventfd failed: " << std::strerror(errno);
        ::close(epFd_);
        epFd_ = -1;
        return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = kWakeupTag;
    ::epoll_ctl(epFd_, EPOLL_CTL_ADD, wakeupFd_, &ev);

    if (cfg_.listenEnable)
    {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listenFd_ < 0)
        {
            LOG_ERROR << "socket failed: " << std::strerror(errno);
            return false;
        }

        int one = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = host16ToNet(cfg_.listenPort);
        if (::inet_pton(AF_INET, cfg_.listenAddr.c_str(), &addr.sin_addr) != 1)
        {
            LOG_ERROR << "bad listen addr: " << cfg_.listenAddr;
            return false;
        }

        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            LOG_ERROR << "bind " << cfg_.listenAddr << ":" << cfg_.listenPort
                      << " failed: " << std::strerror(errno);
            return false;
        }
        if (::listen(listenFd_, 1024) != 0)
        {
            LOG_ERROR << "listen failed: " << std::strerror(errno);
            return false;
        }

        epoll_event lev{};
        lev.events = EPOLLIN;
        lev.data.u64 = kListenTag;
        ::epoll_ctl(epFd_, EPOLL_CTL_ADD, listenFd_, &lev);

        LOG_INFO << "net listen on " << cfg_.listenAddr << ":" << cfg_.listenPort;
    }

    // 注册唤醒：任意逻辑线程 sendToNet 后由本线程的 eventfd 唤醒 epoll
    m_bus_->setNetWakeup([this] { wakeup(); });

    started_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void NetServer::stop()
{
    if (!started_.exchange(false, std::memory_order_acq_rel))
    {
        if (thread_.joinable())
            thread_.join();
        return;
    }

    wakeup();  // 让 epoll_wait 立刻返回并看到 started_ == false
    if (thread_.joinable())
        thread_.join();
}

void NetServer::wakeup()
{
    if (wakeupFd_ < 0)
        return;
    const uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    (void)n;  // eventfd 计数溢出只可能是没人消费，属可忽略场景
}

void NetServer::drainWakeup()
{
    uint64_t v = 0;
    while (::read(wakeupFd_, &v, sizeof(v)) > 0) {}
}

bool NetServer::registerConn(Conn&& conn, uint32_t events)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = conn.session;
    if (::epoll_ctl(epFd_, EPOLL_CTL_ADD, conn.fd, &ev) != 0)
    {
        LOG_ERROR << "epoll_ctl ADD fd=" << conn.fd << " failed: " << std::strerror(errno);
        ::close(conn.fd);
        return false;
    }

    const uint64_t session = conn.session;
    conns_.emplace(session, std::move(conn));
    return true;
}

bool NetServer::connectTo(const PeerConfig& peer)
{
    if (!started_.load(std::memory_order_acquire))
    {
        LOG_ERROR << "connectTo before start()";
        return false;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        LOG_ERROR << "socket failed: " << std::strerror(errno);
        return false;
    }

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = host16ToNet(peer.port);
    if (::inet_pton(AF_INET, peer.addr.c_str(), &addr.sin_addr) != 1)
    {
        LOG_ERROR << "bad peer addr: " << peer.addr;
        ::close(fd);
        return false;
    }

    Conn conn;
    conn.fd = fd;
    conn.session = nextSession_.fetch_add(1, std::memory_order_relaxed);
    conn.peerServiceID = peer.serviceId;
    conn.linkType = LinkTypeOf(peer.serviceId);
    conn.outbound = true;
    conn.connecting = true;

    // 非阻塞 connect：EINPROGRESS 是正常路径，完成后由 EPOLLOUT 通知
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS)
    {
        LOG_ERROR << "connect " << peer.addr << ":" << peer.port
                  << " failed: " << std::strerror(errno);
        ::close(fd);
        return false;
    }

    const uint64_t session = conn.session;
    const uint8_t serviceId = peer.serviceId;

    if (!registerConn(std::move(conn), EPOLLIN | EPOLLET | EPOLLOUT))
        return false;

    LOG_INFO << "connecting to " << ServerIdName(serviceId) << " " << peer.addr << ":" << peer.port
             << " session=" << session;
    return true;
}

void NetServer::run()
{
    epoll_event events[256];

    while (started_.load(std::memory_order_acquire))
    {
        // -1：纯阻塞等待。已有 eventfd 唤醒机制，不需要超时轮询
        const int n = ::epoll_wait(epFd_, events, 256, -1);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR << "epoll_wait failed: " << std::strerror(errno);
            break;
        }

        for (int i = 0; i < n; ++i)
        {
            const uint64_t session = events[i].data.u64;
            const uint32_t ev = events[i].events;

            if (session == kListenTag)
            {
                acceptNewConn();
                continue;
            }
            if (session == kWakeupTag)
            {
                drainWakeup();
                processBusMsg();
                continue;
            }

            auto it = conns_.find(session);
            if (it == conns_.end())
                continue;   // 同一批事件里该连接已被关闭：必须 continue，return 会让整条网络线程退出

            if (it->second.connecting && (ev & (EPOLLOUT | EPOLLHUP | EPOLLERR)))
            {
                onPeerWritable(it->second);
                continue;   // 可能已关闭并删除连接
            }
            if (ev & (EPOLLHUP | EPOLLERR))
            {
                handleClose(it->second);
                continue;
            }
            if (ev & EPOLLIN)
            {
                onPeerReadable(it->second);
                // 读取过程中可能因协议错误/超限关闭连接
                if (conns_.find(session) == conns_.end())
                    continue;
            }
            if (ev & EPOLLOUT)
            {
                auto it2 = conns_.find(session);
                if (it2 == conns_.end())
                    continue;
                doWrite(it2->second);
            }
        }

        // 关停前把队列里剩下的出站消息尽量发出去（例如踢人提示）
        processBusMsg();
    }

    for (auto& kv : conns_)
    {
        if (kv.second.fd >= 0)
            ::close(kv.second.fd);
    }
    conns_.clear();
    sessionWorker_.clear();

    if (listenFd_ >= 0) ::close(listenFd_);
    if (wakeupFd_ >= 0) ::close(wakeupFd_);
    if (epFd_ >= 0) ::close(epFd_);
    listenFd_ = wakeupFd_ = epFd_ = -1;

    LOG_INFO << "net thread stopped";
}

void NetServer::acceptNewConn()
{
    for (;;)
    {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = ::accept4(listenFd_, reinterpret_cast<sockaddr*>(&peer), &len,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                LOG_WARNING << "accept4 failed: " << std::strerror(errno);
            break;   // ET 模式下必须循环到 EAGAIN 才算取空
        }

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

        Conn conn;
        conn.fd = fd;
        conn.session = nextSession_.fetch_add(1, std::memory_order_relaxed);
        // 入站连接的归属由握手确认；默认按网关处理（与部署形态一致）
        conn.peerServiceID = ServerID::kGateway;
        conn.linkType = LinkTypeOf(conn.peerServiceID);

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

        const uint64_t session = conn.session;
        if (!registerConn(std::move(conn), EPOLLIN | EPOLLET))
            continue;

        // 会话亲和：accept 时按"队列积压 + session 哈希打散"挑一个逻辑线程。
        // 它只管会话状态（收包缓冲/认证态）；玩家 Actor 归属仍由 playerId 决定。
        const MsgBus::WorkerId wid = workerForSession(session, true, 0);

        LOG_INFO << "accept conn from " << ip << ":" << peer.sin_port
                 << " session=" << session << " worker=" << wid;

        Msg m;
        m.head.msgType = MsgType::MSGTYPE_CONN_NEW;
        m.head.session = session;
        m.head.ctx = ServerID::kGateway;
        m_bus_->sendToWorker(wid, std::move(m));
    }
}

MsgBus::WorkerId NetServer::workerForSession(uint64_t session, bool assignIfMissing, uint64_t playerId)
{
    auto it = sessionWorker_.find(session);
    if (it != sessionWorker_.end())
        return it->second;

    if (!assignIfMissing)
        return hashutil::WorkerForSession(session, m_bus_->numWorker());

    // 负载依据用"队列积压深度"，而不是连接数：accept 时看不到别的连接，
    // 也看不到 worker 的繁忙程度。同分时按 session 哈希打散，避免羊群效应。
    const MsgBus::WorkerId wid = m_bus_->leastLoadedWorker(session ^ playerId);
    sessionWorker_[session] = wid;
    return wid;
}

void NetServer::onPeerReadable(Conn& conn)
{
    const uint64_t session = conn.session;
    auto& buffer = conn.readBuffer;

    // 1. ET 模式必须循环读到 EAGAIN，否则事件不会再通知 -> 数据永久滞留内核缓冲
    for (;;)
    {
        if (buffer.readableBytes() >= cfg_.maxReadBufferBytes)
        {
            // 背压：对端持续发、逻辑层消费不过来。断开而不是让内存无限增长
            LOG_WARNING << "read buffer overflow, session=" << session
                        << " readable=" << buffer.readableBytes();
            handleClose(conn);
            return;
        }

        int savedErrno = 0;
        const ssize_t n = buffer.readFd(conn.fd, &savedErrno);
        if (n > 0)
            continue;
        if (n == 0)
        {
            // 对端关闭：先把已读到的整帧交给逻辑层，再把连接标记为"只允许回写"，
            // 由下面的统一收尾处关闭 —— 不能只打日志不关，否则连接会一直挂在 conns_ 里，
            // 逻辑层也收不到 CONN_CLOSE（玩家下线清理就不会发生）。
            LOG_INFO << "peer closed, session=" << session;
            conn.peerClosed = true;
            break;
        }
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
            break;
        if (savedErrno == EINTR)
            continue;

        LOG_WARNING << "read failed, session=" << session << " errno=" << savedErrno;
        handleClose(conn);
        return;
    }

    // 2. 拆帧：粘包/半包/脏数据重同步都在 DecodeFrame 内处理
    const int64_t now = NowMs();
    for (;;)
    {
        Header head;
        std::string body;
        const DecodeStatus st = DecodeFrame(buffer, head, cfg_.serviceId, body);

        if (st == DecodeStatus::kNeedMore)
        {
            // 半包 + 对端已关闭：残余不可能再补齐，直接收尾
            if (conn.peerClosed)
                handleClose(conn);
            return;
        }

        if (st == DecodeStatus::kError)
        {
            // 协议错误要计数并告警：可能是攻击，也可能是协议版本不匹配
            if (++conn.decodeErrors >= cfg_.maxDecodeErrors)
            {
                LOG_WARNING << "too many decode errors, close session=" << session;
                handleClose(conn);
                return;
            }
            continue;
        }

        // 3. 帧速率限制：防止单连接刷包把逻辑线程饿死
        if (conn.rateWindowMs == 0 || now - conn.rateWindowMs >= 1000)
        {
            conn.rateWindowMs = now;
            conn.framesInWindow = 0;
        }
        if (++conn.framesInWindow > cfg_.maxFramesPerSec)
        {
            LOG_WARNING << "frame rate exceeded, close session=" << session;
            handleClose(conn);
            return;
        }

        onFrame(conn, head, body);

        // sendTo -> doWrite 有可能因写失败而关闭连接
        if (conns_.find(session) == conns_.end())
            return;
    }
}

void NetServer::onPeerWritable(Conn& conn)
{
    const uint64_t session = conn.session;

    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)
    {
        LOG_WARNING << "connect finished with error, session=" << session
                    << " errno=" << (err != 0 ? err : errno);
        handleClose(conn);
        return;
    }

    conn.connecting = false;

    // 握手：上报本服务身份（serviceType/zoneId/sceneId），网关据此登记 backend 连接
    BackendHandshake hs;
    hs.serviceId = cfg_.serviceId;
    hs.zoneId = cfg_.zoneId;
    hs.sceneId = cfg_.sceneId;

    const std::string body = PackBackendHandshake(hs);
    Buffer frame;
    EncodeFrame(frame,
                MakeHeader(conn.linkType, SysModule::kSYS, SysMethod::kHandshake, 0, 0,
                           cfg_.serviceId, conn.peerServiceID),
                body);
    conn.sendBuffer.append(frame.peek(), frame.readableBytes());

    // 连接已就绪：撤掉 EPOLLOUT 关注，回到常驻的"只关注可读"
    enableWrite(conn, false);
    doWrite(conn);

    if (conns_.find(session) == conns_.end())
        return;

    LOG_INFO << "peer connected, session=" << session << " peer=" << ServerIdName(conn.peerServiceID);

    // 会话亲和：该连接上的会话消息固定落到一个逻辑线程
    const MsgBus::WorkerId wid = workerForSession(session, true, 0);
    Msg m;
    m.head.msgType = MsgType::MSGTYPE_CONN_NEW;
    m.head.session = session;
    m.head.ctx = conn.peerServiceID;
    m_bus_->sendToWorker(wid, std::move(m));
}

void NetServer::onFrame(Conn& conn, const Header& head, const std::string& body)
{
    // 握手是传输层事件，不进逻辑层（也避免逻辑层持有 fd/连接状态）
    if (head.module == SysModule::kSYS && head.method == SysMethod::kHandshake)
    {
        handleHandshake(conn, body);
        return;
    }

    conn.playerId = head.playerId;

    Msg m;
    m.head.msgType = MsgType::MSGTYPE_CONN_DATA;
    m.head.Module = head.module;
    m.head.Method = head.method;
    m.head.seq = head.seq;
    m.head.playerId = head.playerId;
    m.head.session = conn.session;
    m.body = body;

    if (head.playerId != 0)
    {
        // Actor 亲和：正确性由 playerId 保证（同一玩家永远落在同一逻辑线程）
        m_bus_->sendToWorkerByPlayerId(head.playerId, std::move(m));
    }
    else
    {
        // 会话亲和：登录中等尚未绑定玩家的会话消息
        m_bus_->sendToWorker(workerForSession(conn.session, true, 0), std::move(m));
    }
}

void NetServer::handleHandshake(Conn& conn, const std::string& body)
{
    BackendHandshake hs;
    if (!UnpackBackendHandshake(body, hs))
    {
        LOG_WARNING << "bad handshake body, session=" << conn.session;
        return;
    }

    const uint8_t oldPeer = conn.peerServiceID;
    conn.peerServiceID = hs.serviceId;
    conn.linkType = LinkTypeOf(hs.serviceId);
    LOG_INFO << "handshake session=" << conn.session << " peer=" << ServerIdName(oldPeer)
             << " -> " << ServerIdName(hs.serviceId) << " zone=" << hs.zoneId
             << " scene=" << hs.sceneId;
}

void NetServer::processBusMsg()
{
    Msg m;
    while (m_bus_->tryPopNet(m))
    {
        auto it = conns_.find(m.head.session);
        if (it == conns_.end())
            continue;   // 连接已关闭：静默丢弃（回包迟到是常态，不要刷日志）

        if (m.head.msgType == MsgType::MSGTYPE_SEND)
        {
            sendTo(it->second, m);
        }
        else if (m.head.msgType == MsgType::MSGTYPE_CLOSE)
        {
            LOG_INFO << "logic requests close, session=" << m.head.session
                     << " playerId=" << m.head.playerId;
            handleClose(it->second);
        }
    }
}

void NetServer::sendTo(Conn& conn, const Msg& m)
{
    // 1. 先把这一条消息编码成单独的完整帧
    //    绝不能在 conn.sendBuffer 上直接编码：它里面可能还堆着上一轮 EAGAIN 的残余，
    //    那样会把"旧残余 + 新消息"当成一条消息算长度，帧边界永久错乱。
    Buffer frame;
    EncodeFrame(frame,
                MakeHeader(conn.linkType, m.head.Module, m.head.Method, m.head.seq,
                           m.head.playerId, cfg_.serviceId, conn.peerServiceID),
                m.body);

    // 2. 写缓冲上限：慢客户端不能被允许拖垮整个进程内存
    if (conn.sendBuffer.readableBytes() + frame.readableBytes() > cfg_.maxWriteBufferBytes)
    {
        LOG_WARNING << "send buffer overflow, drop msg, session=" << conn.session
                    << " pending=" << conn.sendBuffer.readableBytes();
        return;
    }

    conn.sendBuffer.append(frame.peek(), frame.readableBytes());

    // 3. 顺手写一把；EAGAIN 时由 doWrite 挂上 EPOLLOUT，残余留在缓冲里
    doWrite(conn);
}

void NetServer::doWrite(Conn& conn)
{
    if (conn.fd < 0)
        return;

    auto& buf = conn.sendBuffer;
    while (buf.readableBytes() > 0)
    {
        int savedErrno = 0;
        const ssize_t n = buf.WriteFd(conn.fd, &savedErrno);
        if (n > 0)
            continue;
        if (n < 0 && savedErrno == EINTR)
            continue;
        if (n < 0 && (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK))
        {
            enableWrite(conn, true);   // 等下次 EPOLLOUT，绝不能在这里自旋重试
            return;
        }

        LOG_WARNING << "write failed, session=" << conn.session << " errno=" << savedErrno;
        handleClose(conn);
        return;
    }

    // 写完了：取消写事件关注（注意不能 retrieveAll，那会丢掉"写到一半"的语义）
    enableWrite(conn, false);

    if (conn.peerClosed)
        handleClose(conn);
}

void NetServer::enableWrite(Conn& conn, bool on)
{
    if (conn.fd < 0)
        return;

    // 旧实现写成 if (writing == on) return; 会让"关闭写关注"变成 no-op，
    // 结果是 EPOLLOUT 常驻 -> 每个可写事件都空转一次
    if (conn.writing == on)
        return;

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET | (on ? EPOLLOUT : 0u);
    ev.data.u64 = conn.session;

    if (::epoll_ctl(epFd_, EPOLL_CTL_MOD, conn.fd, &ev) == 0)
        conn.writing = on;
    else
        LOG_WARNING << "epoll_ctl MOD failed, fd=" << conn.fd << " errno=" << errno;
}

void NetServer::handleClose(Conn& conn)
{
    const uint64_t session = conn.session;
    const int fd = conn.fd;
    const uint64_t playerId = conn.playerId;
    const uint8_t peer = conn.peerServiceID;

    auto it = conns_.find(session);
    if (it == conns_.end())
        return;

    if (fd >= 0)
    {
        ::epoll_ctl(epFd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }
    conns_.erase(it);

    // 会话表必须在这里清理：旧实现只增不减，10 万次连接会留下常驻内存
    MsgBus::WorkerId wid = hashutil::WorkerForSession(session, m_bus_->numWorker());
    auto wit = sessionWorker_.find(session);
    if (wit != sessionWorker_.end())
    {
        wid = wit->second;
        sessionWorker_.erase(wit);
    }

    // 连接关闭是"玩家下线的唯一入口"，必须通知到持有该 Actor 的线程：
    // 否则 Actor 不销毁（内存泄漏）、玩家定时器不取消（回调打到已下线玩家）、不触发存档（丢数据）
    Msg m;
    m.head.msgType = MsgType::MSGTYPE_CONN_CLOSE;
    m.head.session = session;
    m.head.playerId = playerId;
    m.head.ctx = peer;

    if (playerId != 0)
        m_bus_->sendToWorkerByPlayerId(playerId, std::move(m));
    else
        m_bus_->sendToWorker(wid, std::move(m));

    LOG_INFO << "close conn, session=" << session << " playerId=" << playerId
             << " peer=" << ServerIdName(peer);
}
