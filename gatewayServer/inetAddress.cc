#include "inetAddress.h"
#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

namespace clearmoon 
{
namespace net {

/**
 * @brief 辅助函数命名空间
 * 
 */
namespace detail 
{
//------------------地址结构体之间的转换------------------
const struct sockaddr* sockAddress(const sockaddr_in& addr4)
{
    return reinterpret_cast<const struct sockaddr*>(&addr4);
}

const struct sockaddr* sockAddress(const sockaddr_in6& addr6)
{
    return reinterpret_cast<const struct sockaddr*>(&addr6);
}

const struct sockaddr_in* sockAddress_In(const sockaddr& addr)
{
    return reinterpret_cast<const sockaddr_in*>(&addr);
}
const struct sockaddr_in6* sockAddress_In6(const sockaddr& addr)
{
    return reinterpret_cast<const sockaddr_in6*>(&addr);
}

//------------------------------------------------------
//Ipv4/Ipv6 转换成字符串
static std::string ipv4ToString(const struct sockaddr_in& addr4)
{
    char buf[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &addr4.sin_addr, buf, sizeof(buf));
    return buf;
}

static std::string ipv6ToString(const struct sockaddr_in6& addr6)
{
    char buf[INET6_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET6, &addr6.sin6_addr, buf, sizeof(buf));
    return buf;
}


//填充Ipv4/Ipv6
static void fillIpv4(const std::string& ip, uint16_t port, struct sockaddr_in& addr4)
{
    ::memset(&addr4, 0, sizeof(struct sockaddr_in));

    addr4.sin_family = AF_INET;
    addr4.sin_port = ::htons(port);

    if(ip.empty())
        addr4.sin_addr.s_addr = ::htonl(INADDR_ANY);
    else
    {
        int ret = ::inet_pton(AF_INET, ip.c_str(), &addr4.sin_addr);
        assert(ret == 1);
    }
        
}

static void fillIpv6(const std::string& ip, uint16_t port, struct sockaddr_in6& addr6)
{
    ::memset(&addr6, 0, sizeof(struct sockaddr_in6));

    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = ::htons(port);

    if(ip.empty())
        addr6.sin6_addr = in6addr_any;
    else
    {
        int ret = ::inet_pton(AF_INET6, ip.c_str(), &addr6.sin6_addr);
        assert(ret == 1);
    }
        

}

}   //detail
}   //net
}   //clearmoon


using namespace clearmoon;
using namespace net;

// ----------------InetAddress类的实现------------------
//
InetAddress::InetAddress() : ipv6_(false)
{
    ::memset(&addr4_, 0, sizeof(struct sockaddr_in));
}


InetAddress::InetAddress(const std::string& ip, uint16_t port, bool ipv6) : ipv6_(ipv6)
{
    if(ipv6_)
        detail::fillIpv6(ip, port, addr6_);
    else
        detail::fillIpv4(ip, port, addr4_);
}

    //由sockaddr_in/由sockaddr_in6构造
InetAddress::InetAddress(const struct sockaddr_in& addr4) : ipv6_(false)
{
    addr4_ = addr4;
}
InetAddress::InetAddress(const struct sockaddr_in6& addr6) : 
ipv6_(true)
{
    addr6_ = addr6;
}

// ---------------------------------------------------
//将Ip地址/端口号转换成字符串输出
std::string InetAddress::toIp() const
{
    if(ipv6_)
        return detail::ipv6ToString(addr6_);
    else
        return detail::ipv4ToString(addr4_);
        
}

uint16_t InetAddress::toPort() const
{
    if(ipv6_)
        return ntohs(addr6_.sin6_port);
    else
        return ntohs(addr4_.sin_port);
}

std::string InetAddress::toIpPort() const
{
    std::string ip = toIp();
    int port = toPort();
    
    if(ipv6_)
        return "[" + ip + "]:" + std::to_string(port);
    else
        return ip + ":" + std::to_string(port);
}

//获取协议族与Ip地址(sockaddr*)
sa_family_t InetAddress::getFamily() const
{
    if(ipv6_) 
        return addr6_.sin6_family;
    else
        return addr4_.sin_family;
}

const struct sockaddr* InetAddress::getAddress() const
{
    if(ipv6_)
        return detail::sockAddress(addr6_);
    else
        return detail::sockAddress(addr4_);
}


void InetAddress::setFromSockaddr(const sockaddr_storage& addr)
{
    if(addr.ss_family == AF_INET)
    {
        addr4_ = *reinterpret_cast<const sockaddr_in*>(&addr);
        ipv6_ = false;
    }
    else {
        addr6_ = *reinterpret_cast<const sockaddr_in6*>(&addr);
        ipv6_ = true;
    }
}

// len 未使用：地址族（sa_family）已唯一决定按 IPv4/IPv6 解释，结构体长度由类型保证。
// 保留该参数是为了与 accept/getsockname 系列的调用签名保持一致。
void InetAddress::setFromSockaddr(const sockaddr& addr, socklen_t /*len*/)
{
    if(addr.sa_family == AF_INET)
    {
        addr4_ = *reinterpret_cast<const sockaddr_in*>(&addr);
        ipv6_ = false;
    }
    else {
        addr6_ = *reinterpret_cast<const sockaddr_in6*>(&addr);
        ipv6_ = true;
    }
}