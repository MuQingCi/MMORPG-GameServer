#include "net_server.h"
#include "log/logger.h"
#include "common/buffer.h"
#include "common/utils.h"
#include "common/msg.h"
#include "common/proto.h"

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

static constexpr uint64_t kListenMagic = ~0ULL;
static constexpr uint64_t kWeakupMagic = ~0ULL - 1;
static constexpr uint8_t  kMessageType = kSceneMsg;

NetServer::NetServer(const std::string& addr, 
                     uint16_t port,
                     MsgBus& bus)
                     :addr_(addr),
                      port_(port),
                      mBus_(&bus)
{

}

NetServer::~NetServer()
{
    stop();
}

bool NetServer::start()
{
    if(started_) return true;
    started_ = true;

    listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(listenFd_ < 0)
    {
        LOG_ERROR << "create listenFd failed!";
        started_ = false;
        return false;
    }

    ssize_t one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in listenAddr;
    memset(&listenAddr, 0, sizeof(sockaddr_in));

    listenAddr.sin_family = AF_INET;
    listenAddr.sin_port   = host16ToNet(port_);
    inet_pton(AF_INET, addr_.c_str(), &listenAddr.sin_addr);

    ssize_t ret = bind(listenFd_, (sockaddr*)&listenAddr, sizeof listenAddr);
    if(ret != 0)
    {
        LOG_ERROR<<"bind listenFd failed!";
        started_ = false;
        return false;
    }
    listen(listenFd_, 1024);

    epFd_ = epoll_create1(0);
    weakupFd_ = eventfd(0,EFD_NONBLOCK);

    epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.fd  = listenFd_;
    ev.data.u64 = kListenMagic;
    epoll_ctl(epFd_, EPOLL_CTL_ADD, listenFd_, &ev);

    ev.data.fd  = weakupFd_;
    ev.data.u64 = kWeakupMagic;
    epoll_ctl(epFd_, EPOLL_CTL_ADD, weakupFd_, &ev);

    thread_ = std::thread([this]  { run(); });
    return true;
}

void NetServer::stop()
{
    if(!started_) return;

    started_ = false;

    if(thread_.joinable())
        thread_.join();
}


void NetServer::enableWrite(Conn& conn, bool on)
{
    // auto it = conns_.find(session);
    // if(it == conns_.end()) return;
    
    // auto& conn = it->second;
    if(conn.writing == on) return;

    epoll_event ev;
    ev.events   = EPOLLIN | EPOLLET | (on? EPOLLOUT : 0u);
    ev.data.u64 = conn.session;

    if(::epoll_ctl(epFd_, EPOLL_CTL_MOD, conn.fd, &ev) == 0)
        conn.writing = true;
}


void NetServer::run()
{
    struct epoll_event events[256];
    while (started_) {
        int n = epoll_wait(epFd_, events, 256, 200);

        if(n < 0)
        {
            if(errno == EINTR) continue;
            break;
        }

        for(int i = 0; i < n; ++i)
        {
            uint64_t session = events[i].data.u64;
            int fd = events[i].data.fd;
            int ev = events[i].events;
            if(session == kListenMagic)         //监听套接字
            {
                acceptNewConn();
                continue;
            }
            else if(session == kWeakupMagic)    //唤醒
            {
                uint64_t v;
                while(read(weakupFd_, &v, sizeof v) > 0){}
                processBusMsg();
                continue;
            }

            auto it = conns_.find(session);
            if(it == conns_.end()) 
                continue;
            auto& conn = it->second;

            if(ev & (EPOLLHUP | EPOLLERR))    //挂起/错误
            {
                handleClose(conn);
                continue;;
            }
            if(events[i].events & EPOLLIN) //可读事件
            {
                handleRead(conn);
            }
            if(ev & EPOLLOUT)   //可写事件
            {
                handleWrite(conn);
            } 
        }
    }
    // 线程退出, 关闭所有连接
    for (auto& kv : conns_) close(kv.second.fd);
    conns_.clear();
    if (listenFd_ >= 0) close(listenFd_);
    if (epFd_ >= 0) close(epFd_);
    if (weakupFd_ >= 0) close(weakupFd_);
    LOG_INFO<<"net thread stop!";
}


void NetServer::acceptNewConn()
{
    for(;;)
    {
        sockaddr_in peer;
        socklen_t len = sizeof peer;

        int fd = accept4(listenFd_, (sockaddr*)&peer, &len, SOCK_NONBLOCK);
        if(fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            break;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        Conn c;
        c.fd = fd;
        c.session = nextSession_.fetch_add(1,std::memory_order_relaxed);

        conns_[c.session] = std::move(c);

        uint64_t session = c.session;
        epoll_event ev;
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        ev.data.u64 = session;
        epoll_ctl(epFd_, EPOLL_CTL_ADD, fd, &ev);

        

        //TODO 通知逻辑层有新连接
        Msg m;
        m.head.msgType  = MsgType::MSGTYPE_CONN_NEW;
        
        m.head.session  = session;
        // mBus_->sendToActor(ActorId id, Msg m)
        //mBus_->sendToActor(m.head.msgType, m)
    }
}

void NetServer::processBusMsg()
{
    if(mBus_ == nullptr) return;

    Msg m;
    while(mBus_->tryPopNet(m))
    {
        //TODO 处理消息
        auto sid = m.head.session;
        auto it = conns_.find(sid);
        if(it == conns_.end()) continue;
        
        auto& conn = it->second;

        doWrite(conn);
    }
}

void NetServer::doWrite(Conn& conn)
{
    int fd = conn.fd;
    if(fd < 0) return;

    auto& buf = conn.sendBuffer;

    //TODO
    //head内部后面填充
    //kMessageType暂时以当前线程的kSceneMsg;后续修改
    Header head;
    encode(buf, kMessageType, head);

    while (buf.readableBytes() > 0) 
    {
        ssize_t n = ::write(fd, conn.sendBuffer.peek(), buf.readableBytes());
        if (n > 0) {
            buf.retrieve(static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) 
            continue;
        
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            enableWrite(conn, true);
            return;
        }
        handleClose(conn);
        return;
    }

    // 全部写完
    buf.retrieveAll();
    //取消写事件关注
    enableWrite(conn, false);

    if (conn.peer_closed)
        handleClose(conn);
}

void NetServer::handleRead(Conn& conn)
{
    auto& buffer = conn.readBuffer;
    //TODO 网关层发送什么格式的消息到场景服务器
    for (;;) {
        int savedErrno = 0;
        ssize_t n = buffer.readFd(conn.fd, &savedErrno);
        if (n > 0) continue;                       // 还有数据，继续读
        if (n == 0) { handleClose(conn); return; } // 对端关闭
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) break; // 读完
        if (savedErrno == EINTR) continue;
        handleClose(conn); return;
    }

    Header head;
    while(buffer.readableBytes() >= kHeaderSize)   // 粘包/半包在这里处理
    {
        std::string body;
        if(!decode(buffer, head, kMessageType,body)) 
            break;
        Msg m;
        m.head.Module = head.module;
        m.head.Method = head.method;
        m.head.playerId = head.playerId;
        m.head.seq    = head.seq;
        m.head.session= conn.session;

        // auto it = playerWorker_.find(head.playerId);
        // if(it == playerWorker_.end())
        //     //随机赋值给其一个wid/将只负责处理玩家业务的逻辑线程id赋值给它
        //     playerWorker_[head.playerId] = 1;
        
        // auto wid = playerWorker_[head.playerId];
        if(!session_.count(conn.session))
            session_[conn.session] = conn.playerId;
        mBus_->sendToWorkerByPlayerId(m.head.playerId, m);
    }
}

void NetServer::handleWrite(Conn& conn)
{
    while(conn.sendBuffer.readableBytes() > 0)
    {
        ssize_t n = ::write(conn.fd, conn.sendBuffer.peek(), conn.sendBuffer.readableBytes());

        if(n > 0)
        {
            conn.sendBuffer.retrieve(static_cast<size_t>(n));
            continue;
        }
        
        if (n < 0 && errno == EINTR) 
            continue;
        
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        handleClose(conn);
        return;
    }
    enableWrite(conn, false);
}

void NetServer::handleClose(Conn& conn)
{
    if(!conn.peer_closed)
    {
        conn.peer_closed = true;

        //TODO 通知逻辑线程连接关闭
        Msg m;
        m.head.msgType = MsgType::MSGTYPE_CONN_CLOSE;
        m.head.session = conn.session;
        m.head.playerId = session_[conn.session];

        mBus_->sendToWorker(session_[conn.session], m);
    }

    ::epoll_ctl(epFd_, EPOLL_CTL_DEL, conn.fd, nullptr);
    conns_.erase(conn.session);
    ::close(conn.fd);
}
