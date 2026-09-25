#include "acceptor.h"
#include "inetAddress.h"

#include <cerrno>
#include <sys/socket.h>

Acceptor::Acceptor(EventLoop* loop, const InetAddress& addr,connRole role) 
                 : loop_(loop), 
                   listenSock_(AF_INET), acceptChannel_(loop, listenSock_.Fd()),
                   role_(role)
{
    listenSock_.setReuseAddress(true);
    listenSock_.setNonBlocking(true);
    listenSock_.bind(addr);
    // 同线程回调：channel 的读事件由本 EventLoop 分发，Acceptor 的生命周期由 GatewayServer 保证
    acceptChannel_.setReadCallback([this]{ handleRead(); });
}

Acceptor::~Acceptor()
{
    acceptChannel_.disableAll();
}

void Acceptor::listen()
{
    if(listening_) 
        return;
    listenSock_.listen();
    acceptChannel_.enableReading();
    listening_ = true;
}

void Acceptor::handleRead()
{
    while (true) //循环接受新连接
    {
        InetAddress peerAddr;
        Socket conn = listenSock_.accept(&peerAddr);
        if(conn.valid())
        {
            if(connCallback_)
                connCallback_(std::move(conn), std::move(peerAddr), role_);
        }
        
        else {
            int savedErrno = errno;
            if(savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
            {
                break;
            }
        }
    }
    
}

void Acceptor::close()
{
    if(!listening_) return;
    // 投递到 loop 执行：当前调用方就是 loop 所在线程（runInLoop 会就地执行），捕获裸 this 安全。
    // TODO 若将来允许从其它线程调用 close()，这里需要改为可延长生命周期的句柄（见阶段 6 停服顺序）。
    loop_->runInLoop([this] {
        acceptChannel_.disableAll();
        listening_ = false;
     });
    
}
