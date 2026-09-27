#ifndef CLEARMOON_GATEWAY_TESTS_E2E_CONN_H
#define CLEARMOON_GATEWAY_TESTS_E2E_CONN_H

// ============================================================================
// 端到端测试用的阻塞式连接封装（只给 tests/e2e_client.cc 使用）
//
// 为什么不用 gateway_core 的 EventLoop/Reactor：
//   本工具要扮演的是**对端**（客户端 / 场景服），必须在网关进程之外、用最直接的方式
//   收发字节。用阻塞 socket + poll 超时，能保证"网关不回包时测试失败而不是挂住"。
// ============================================================================
#include "base/buffer.h"
#include "base/clientProto.h"
#include "base/proto.h"

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace e2e
{

struct Conn
{
    int fd = -1;
    Buffer in;            // 已读入但尚未组帧的字节

    // 本端扮演的**服务号**：服务间解码必须用它做过滤（网关转发来的帧 dst=目标服务号）。
    // 本工具扮演场景服，因此默认 SCENE_1；如果用来扮演网关/别的服务，改这个字段即可。
    uint8_t selfServiceId = ServerID::kScene_1;

    Conn() = default;
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    ~Conn()
    {
        if (fd >= 0)
            ::close(fd);
    }

    bool open(uint16_t port)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            fd = -1;
            return false;
        }

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return true;
    }

    bool sendRaw(const std::string& s)
    {
        size_t off = 0;
        while (off < s.size())
        {
            const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n > 0)
            {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        return true;
    }

    // ---------------- 组帧发送 ----------------
    bool sendClientFrame(ClientKind kind, uint16_t module, uint16_t method, uint64_t requestId,
                         const std::string& body)
    {
        ClientHeader h;
        h.kind = kind;
        h.module = module;
        h.method = method;
        h.requestId = requestId;
        Buffer out;
        EncodeClientFrame(out, h, body);
        return sendRaw(std::string(out.peek(), out.readableBytes()));
    }

    bool sendGateFrame(uint8_t msgType, uint16_t module, uint16_t method, uint64_t seq,
                       uint64_t playerId, uint8_t src, uint8_t dst, const std::string& body)
    {
        Buffer out;
        EncodeFrame(out, MakeHeader(msgType, module, method, seq, playerId, src, dst), body);
        return sendRaw(std::string(out.peek(), out.readableBytes()));
    }

    // ---------------- 收数据 ----------------
    // 返回：1 = 读到字节；0 = 对端关闭；-1 = 超时/错误
    int pumpOnce(int timeoutMs)
    {
        pollfd p{};
        p.fd = fd;
        p.events = POLLIN;
        const int pr = ::poll(&p, 1, timeoutMs);
        if (pr == 0)
            return -1;
        if (pr < 0)
            return (errno == EINTR) ? 1 : -1;

        int savedErrno = 0;
        const ssize_t n = in.readFd(fd, &savedErrno);
        if (n > 0)
            return 1;
        if (n == 0)
            return 0;   // 对端关闭
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK || savedErrno == EINTR)
            return 1;
        return -1;
    }

    // 读一帧客户端帧（循环读+解码，直到整帧到齐或超时）
    bool readClientFrame(ClientHeader& h, std::string& body, int timeoutMs)
    {
        for (;;)
        {
            const DecodeStatus st = DecodeClientFrame(in, h, body);
            if (st == DecodeStatus::kOk)
                return true;
            if (st == DecodeStatus::kError)
                return false;
            const int r = pumpOnce(timeoutMs);
            if (r <= 0)
                return false;
        }
    }

    bool readGateFrame(Header& h, std::string& body, int timeoutMs)
    {
        for (;;)
        {
            const DecodeStatus st = DecodeFrame(in, h, selfServiceId, body);
            if (st == DecodeStatus::kOk)
                return true;
            if (st == DecodeStatus::kError)
                return false;
            const int r = pumpOnce(timeoutMs);
            if (r <= 0)
                return false;
        }
    }

    // 期望"对端把本连接断开"（网关的 fail-closed 行为）
    bool expectClosedByPeer(int timeoutMs)
    {
        for (;;)
        {
            const int r = pumpOnce(timeoutMs);
            if (r == 0)
                return true;
            if (r < 0)
                return false;
            // 还有数据：可能是断开前的错误帧，继续读到 EOF
        }
    }
};

}  // namespace e2e

#endif
