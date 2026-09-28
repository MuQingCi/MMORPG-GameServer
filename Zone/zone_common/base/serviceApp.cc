#include "base/serviceApp.h"
#include "base/clientProto.h"

#include "log/logger.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
std::atomic<bool> running{true};
void Stop(int) { running.store(false); }

struct Message { Header header; std::string body; size_t gateway = 0; };

class Queue
{
public:
    static constexpr size_t kCapacity = 4096;

    bool Put(Message m)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (items_.size() >= kCapacity) return false;
        items_.push_back(std::move(m));
        cv_.notify_one();
        return true;
    }
    // 响应与所有网关推送要么全部排队，要么全部拒绝。
    bool PutBatch(std::vector<Message> batch)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (batch.size() > kCapacity - items_.size()) return false;
        for (auto& item : batch) items_.push_back(std::move(item));
        cv_.notify_one();
        return true;
    }
    bool Get(Message& m)
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return !items_.empty() || !running.load(); });
        if (items_.empty()) return false;
        m = std::move(items_.front());
        items_.pop_front();
        return true;
    }
    bool TryGet(Message& m)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (items_.empty()) return false;
        m = std::move(items_.front());
        items_.pop_front();
        return true;
    }
private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Message> items_;
};

// 拒绝控制字符、非法 UTF-8、代理区码点以及非最短编码；文本不允许嵌入 NUL。
bool ValidChatText(const std::string& text)
{
    if (text.empty() || text.size() > 1024) return false;
    for (size_t i = 0; i < text.size();)
    {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        if (c < 0x20 || c == 0x7f) return false;
        size_t n = 0;
        if (c < 0x80) n = 1;
        else if (c >= 0xc2 && c <= 0xdf) n = 2;
        else if (c >= 0xe0 && c <= 0xef) n = 3;
        else if (c >= 0xf0 && c <= 0xf4) n = 4;
        else return false;
        if (i + n > text.size()) return false;
        for (size_t j = 1; j < n; ++j)
            if ((static_cast<uint8_t>(text[i + j]) & 0xc0) != 0x80) return false;
        if (n > 1)
        {
            const uint8_t second = static_cast<uint8_t>(text[i + 1]);
            if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
                (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) return false;
        }
        i += n;
    }
    return true;
}

void Logic(Queue& input, Queue& output, uint8_t id, size_t gatewayCount)
{
    while (running.load())
    {
        Message msg;
        if (!input.Get(msg)) continue;
        const Header& h = msg.header;
        if (h.seq == 0 || h.playerId == 0) continue; // 网络层已阻止非法请求
        const uint8_t linkType = id == ServerID::kChat ? NetMessageType::kChatMsg
                                                        : NetMessageType::kGlobalMsg;
        auto reply = [&](std::string body) {
            if (!output.Put({MakeHeader(linkType, h.module, h.method, h.seq,
                                        h.playerId, id, ServerID::kGateway), std::move(body), msg.gateway}))
                running.store(false); // 响应无法排队时断开链路，不让请求静默超时
        };
        if (id == ServerID::kChat && h.module == ServiceModule::kChat &&
            h.method == ServiceMethod::kPrivateChat)
        {
            uint64_t target = 0;
            std::string text, name;
            if (!ParseChatBody(msg.body, target, name, text) || target == 0 ||
                target == h.playerId || name.empty() || !ValidChatText(text))
            {
                reply("invalid chat message");
                continue;
            }
            const std::string pushBody = ChatBody(h.playerId, name, text);
            std::vector<Message> batch;
            batch.reserve(gatewayCount + 1);
            batch.push_back({MakeHeader(linkType, h.module, h.method, h.seq,
                                        h.playerId, id, ServerID::kGateway), "accepted", msg.gateway});
            for (size_t i = 0; i < gatewayCount; ++i)
                batch.push_back({MakeHeader(linkType, h.module, h.method, 0,
                                            target, id, ServerID::kGateway), pushBody, i});
            if (!output.PutBatch(std::move(batch)))
                reply("chat queue full");
        }
        else if (id == ServerID::kChat && h.module == ServiceModule::kChat &&
                 h.method == ServiceMethod::kZoneShout)
        {
            std::string name, text;
            uint64_t ignored = 0;
            if (!ParseChatBody(std::string(8, '\0') + msg.body, ignored, name, text) ||
                !ValidChatText(text))
            {
                reply("invalid chat message");
                continue;
            }
            const std::string pushBody = ChatBody(h.playerId, name, text);
            std::vector<Message> batch;
            batch.reserve(gatewayCount + 1);
            batch.push_back({MakeHeader(linkType, h.module, h.method, h.seq,
                                        h.playerId, id, ServerID::kGateway), "accepted", msg.gateway});
            for (size_t i = 0; i < gatewayCount; ++i)
                batch.push_back({MakeHeader(linkType, h.module, h.method, 0,
                                            0, id, ServerID::kGateway), pushBody, i});
            if (!output.PutBatch(std::move(batch))) reply("chat queue full");
        }
        else if (id == ServerID::kChat && h.module == ServiceModule::kChat &&
                 h.method == ServiceMethod::kAdminBroadcast)
        {
            reply("admin broadcast unavailable");
        }
        else if (id == ServerID::kGlobal && h.module == ServiceModule::kGlobal &&
                 h.method == ServiceMethod::kGlobalPing)
        {
            reply(std::move(msg.body));
        }
        else
        {
            // 网关回程校验要求响应保留 module/method/playerId/seq。
            reply("unsupported method");
        }
    }
}

bool WritePending(int fd, Buffer& out)
{
    while (out.readableBytes())
    {
        const ssize_t n = ::send(fd, out.peek(), out.readableBytes(), MSG_NOSIGNAL);
        if (n > 0) { out.retrieve(static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    return true;
}
} // namespace

std::string ChatBody(uint64_t playerId, const std::string& name, const std::string& text)
{
    if (name.empty() || !ValidRoleName(name)) return {};
    std::string out(8, '\0');
    for (int i = 0; i < 8; ++i) out[i] = static_cast<char>(playerId >> (56 - i * 8));
    out += name;
    out.append(kRoleNameSize - name.size(), '\0');
    out += text;
    return out;
}

bool ParseChatBody(const std::string& body, uint64_t& playerId, std::string& name, std::string& text)
{
    if (body.size() < 8 + kRoleNameSize) return false;
    playerId = 0;
    for (int i = 0; i < 8; ++i)
        playerId = (playerId << 8) | static_cast<unsigned char>(body[i]);
    size_t n = 0;
    while (n < kRoleNameSize && body[8 + n] != '\0') ++n;
    for (size_t i = n; i < kRoleNameSize; ++i)
        if (body[8 + i] != '\0') return false;
    name.assign(body, 8, n);
    if (name.empty() || !ValidRoleName(name)) return false;
    text.assign(body, 8 + kRoleNameSize, std::string::npos);
    return true;
}

int RunService(int argc, char** argv, uint8_t id)
{
    if (argc < 4 || (argc - 1) % 3 != 0 || (id != ServerID::kChat && argc != 4))
    {
        std::fprintf(stderr, "usage: %s <gateway private IPv4> <port> <zoneId> [<IPv4> <port> <zoneId> ...]\n", argv[0]);
        return 2;
    }
    const size_t count = static_cast<size_t>((argc - 1) / 3);
    if (count + 1 > Queue::kCapacity) return 2;
    struct Link { int fd = -1; Buffer input; Buffer output; };
    std::vector<Link> links(count);
    uint32_t zoneId = 0;
    auto closeLinks = [&] { for (auto& link : links) if (link.fd >= 0) ::close(link.fd); };
    for (size_t i = 0; i < count; ++i)
    {
        char* end = nullptr;
        const unsigned long port = std::strtoul(argv[3 * i + 2], &end, 10);
        if (*end || port == 0 || port > 65535) { closeLinks(); return 2; }
        const unsigned long zone = std::strtoul(argv[3 * i + 3], &end, 10);
        if (*end || zone == 0 || zone > UINT32_MAX || (zoneId != 0 && zone != zoneId))
            { closeLinks(); return 2; }
        zoneId = static_cast<uint32_t>(zone);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, argv[3 * i + 1], &addr.sin_addr) != 1)
            { closeLinks(); return 2; }
        // 禁止重复配置同一网关，否则单一玩家会收到重复推送。
        for (size_t j = 0; j < i; ++j)
            if (argv[3 * i + 1] == std::string(argv[3 * j + 1]) &&
                port == std::strtoul(argv[3 * j + 2], nullptr, 10))
                { closeLinks(); return 2; }
        links[i].fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (links[i].fd < 0 || ::connect(links[i].fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            std::fprintf(stderr, "connect gateway failed: %s\n", std::strerror(errno));
            closeLinks();
            return 1;
        }
        ::fcntl(links[i].fd, F_SETFL, ::fcntl(links[i].fd, F_GETFL, 0) | O_NONBLOCK);
        EncodeFrame(links[i].output,
                    MakeHeader(id == ServerID::kChat ? NetMessageType::kChatMsg : NetMessageType::kGlobalMsg,
                               SysModule::kSYS, SysMethod::kHandshake, 0, 0, id, ServerID::kGateway),
                    PackBackendHandshake({id, zoneId, 0}));
    }
    std::signal(SIGTERM, Stop);
    std::signal(SIGINT, Stop);
    Queue toLogic, toNet;
    std::thread worker([&] { Logic(toLogic, toNet, id, count); });
    bool ok = true;
    while (running.load() && ok)
    {
        Message m;
        while (toNet.TryGet(m))
        {
            Buffer& out = links[m.gateway].output;
            if (out.readableBytes() + m.body.size() + kHeaderSize > 4 * 1024 * 1024)
                { ok = false; break; }
            EncodeFrame(out, m.header, m.body);
        }
        if (!ok) break;
        std::vector<pollfd> fds;
        fds.reserve(count);
        for (auto& link : links)
            fds.push_back({link.fd, static_cast<short>(POLLIN | (link.output.readableBytes() ? POLLOUT : 0)), 0});
        const int n = ::poll(fds.data(), fds.size(), 20);
        if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
        for (size_t i = 0; i < count && ok; ++i)
        {
            auto& link = links[i];
            if (fds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) { ok = false; break; }
            if (fds[i].revents & POLLOUT) ok = WritePending(link.fd, link.output);
            if (!ok || !(fds[i].revents & POLLIN)) continue;
            char bytes[8192];
            const ssize_t r = ::recv(link.fd, bytes, sizeof(bytes), 0);
            if (r == 0) { ok = false; break; }
            if (r < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ok = false; continue; }
            link.input.append(bytes, static_cast<size_t>(r));
            if (link.input.readableBytes() > 1024 * 1024) { ok = false; break; }
            while (ok)
            {
                Header h;
                std::string body;
                const auto st = DecodeFrame(link.input, h, id, body);
                if (st == DecodeStatus::kNeedMore) break;
                if (st == DecodeStatus::kError) { ok = false; break; }
                if (h.srcServiceID != ServerID::kGateway || h.seq == 0 || h.playerId == 0 ||
                    !toLogic.Put({h, std::move(body), i})) { ok = false; break; }
            }
        }
    }
    running.store(false);
    worker.join();
    closeLinks();
    return ok ? 0 : 1;
}