#ifndef CLEARMOON_GATEWAY_INETADDRESS_H
#define CLEARMOON_GATEWAY_INETADDRESS_H

#include "common/copy.h"
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

class InetAddress : public copyable
{
public:
    //默认为:"0.0.0.0"
    explicit InetAddress();

    //由ip,port构造
    InetAddress(const std::string& ip, uint16_t port, bool ipv6);

    //由sockaddr_in/由sockaddr_in6构造
    InetAddress(const struct sockaddr_in& addr4);
    InetAddress(const struct sockaddr_in6& addr6);

    //拷贝构造
    InetAddress(const InetAddress& other) : ipv6_(other.ipv6_)
    {
        if(ipv6_)
            addr6_ = other.addr6_;
        else
            addr4_ = other.addr4_;
    }

    InetAddress& operator=(const InetAddress& other)
    {
        if(this != &other)
        {
            ipv6_ = other.ipv6_;
            if(ipv6_)
                addr6_ = other.addr6_;
            else
                addr4_ = other.addr4_;
        }
        return *this;
    }


    //移动构造
    InetAddress(InetAddress&& other) noexcept
        : ipv6_(other.ipv6_)
    {
        if(ipv6_)
        {
            addr6_ = other.addr6_;
        }
        else
        {
            addr4_ = other.addr4_;
        }
    }
    InetAddress& operator=(InetAddress&& other) noexcept
    {
        if(this != &other)
        {
            ipv6_ = other.ipv6_;
            if(ipv6_)
            {
                addr6_ = other.addr6_;
            }
            else
            {
                addr4_ = other.addr4_;
            }
        }
        return *this;
    }

    //将Ip地址/端口号转换成字符串输出
    std::string toIp() const;
    uint16_t toPort() const;
    std::string toIpPort() const;

    sa_family_t getFamily() const;
    const struct sockaddr* getAddress() const;

    void setFromSockaddr(const sockaddr_storage& addr);
    void setFromSockaddr(const sockaddr& addr, socklen_t len);

    //获取地址长度
    socklen_t getSockLen() const
    {
        if(ipv6_)
            return static_cast<socklen_t>(sizeof(struct sockaddr_in6));
        else
            return static_cast<socklen_t>(sizeof(sockaddr_in));
    }
private:
    union
    {
        struct sockaddr_in addr4_;
        struct sockaddr_in6 addr6_ ;    
    };

    bool ipv6_; //标志是否使用ipv6
};

#endif