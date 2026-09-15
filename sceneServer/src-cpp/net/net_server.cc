#include "net_server.h"
#include "common/utils.h"

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <utility>

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
        //TODO 打日志
        started_ = false;
        return false;
    }

    ssize_t one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in listenAddr;
    memset(&listenAddr, 0, sizeof(sockaddr_in));

    listenAddr.sin_family = AF_INET;
    listenAddr.sin_port   = host16ToNet(port_);
    inet_pton(AF_INET, addr_.c_str(), &listenAddr.sin_addr);

    ssize_t ret = bind(listenFd_, (sockaddr*)&listenAddr, sizeof listenAddr);
    if(ret != 0)
    {
        //TODO 打日志
        started_ = false;
        return false;
    }
    listen(listenFd_, 1024);

    epFd_ = epoll_create1(0);
    weakupFd_ = eventfd(0,EFD_NONBLOCK);

    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listenFd_;
    epoll_ctl(epFd_, EPOLL_CTL_ADD, listenFd_, &ev);

    ev.data.fd = weakupFd_;
    epoll_ctl(epFd_, EPOLL_CTL_ADD, weakupFd_, &ev);

    thread_ = std::thread([this]  { run(); });
    return true;
}

void NetServer::stop()
{
    if(!started_) return;

    started_ = true;

    if(thread_.joinable())
        thread_.join();
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
            int fd = events[i].data.fd;
            if(fd == listenFd_)         //监听套接字
            {
                acceptNewConn();
            }
            else if(fd == weakupFd_)    //唤醒
            {
                uint64_t v;
                while(read(weakupFd_, &v, sizeof v) > 0){}

            }
            else if(events[i].events & EPOLLIN) //可读事件
            {
                for(auto& kv : conns_)
                {
                    if(kv.second.fd == fd)
                    {
                        handleRead();
                        break;
                    }
                }
            }
            else if(events[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR))    //可写/挂起/错误
            {
                for (auto& kv : conns_) 
                {
                    if (kv.second.fd == fd) 
                    {
                        if (events[i].events & EPOLLOUT)  //TODO handleWrite()写消息;
                        if (events[i].events & (EPOLLHUP | EPOLLERR)) 
                        {
                            if (!kv.second.peer_closed) {
                                kv.second.peer_closed = true;
                                // TODO 通知逻辑层
                                // Msg m;
                                // m.head.type = MSGTYPE_CONN_CLOSE;
                                // m.head.session = kv.second.id;
                                // m.h.src = THREAD_NET;
                                // m.h.dst = THREAD_LOGIC;
                                // PushLogicMsg(std::move(m));
                            }
                        }
                    }
                    break;
                }
            }
        }
    }
    // 线程退出, 关闭所有连接
    for (auto& kv : conns_) close(kv.second.fd);
    conns_.clear();
    if (listenFd_ >= 0) close(listenFd_);
    if (epFd_ >= 0) close(epFd_);
    if (weakupFd_ >= 0) close(weakupFd_);
    //TODO LOG("net thread stop");
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
        c.id = nextId_.fetch_add(1,std::memory_order_relaxed);

        conns_[c.id] = std::move(c);

        epoll_event ev;
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(epFd_, EPOLL_CTL_ADD, fd, &ev);

        //TODO 通知逻辑层有新连接
    }
}

void NetServer::handleRead()
{
    //TODO 网关层发送什么格式的消息到场景服务器
}

void NetServer::handleWrite()
{

}

void NetServer::handleClose()
{

}
