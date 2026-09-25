#ifndef CLEARMOON_GATEWAY_SOCKET_H
#define CLEARMOON_GATEWAY_SOCKET_H

#include "base/noncopy.h"
#include "inetAddress.h"
#include <sys/socket.h>

class Socket : public noncopyable
{
public:
    Socket() noexcept;
    Socket(int domain, int type = SOCK_STREAM, int protocol = 0);
    static Socket fromFd(int sockfd);

    //移动构造
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    ~Socket();

    InetAddress getLocalAddr() const;

    //socket三步固定流程
    void bind(const InetAddress& addr);
    void listen(int num = SOMAXCONN);
    Socket accept(InetAddress* peerAddr = nullptr);

    //设置socket相关选项
    void setReuseAddress(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);
    void setNonBlocking(bool on);
    void setTcpNoDelay(bool on);

    //关闭写端
    void shutdownWrite();

    int Fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
private:
    explicit Socket(int sockfd, bool ignored);
    int fd_;
    
};

#endif