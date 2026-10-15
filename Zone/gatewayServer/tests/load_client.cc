#include "loadStats.h"
#include "metricsReader.h"

#include "base/buffer.h"
#include "base/clientProto.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <fstream>
#include <sstream>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    std::string host = "127.0.0.1";
    uint16_t port = 10000;
    uint32_t connections = 1;
    uint32_t rpsPerConnection = 10;
    uint32_t durationSec = 10;
    uint64_t playerBase = 100000;
    uint64_t ticket = 998877;
    uint16_t module = 1;
    uint16_t method = ClientSysMethod::kPing;
    std::string body;
    std::string output = "load-summary.json";
    bool ping = true;
    bool scene = false;
    std::string gatewayMetrics;
    std::string sceneMetrics;
    std::string metricsOutput;
    uint32_t metricsIntervalSec = 1;
};

void Usage(const char* program)
{
    std::cerr << "usage: " << program << " [options]\n"
              << "  --host HOST                 default 127.0.0.1\n"
              << "  --port PORT                 default 10000\n"
              << "  --connections N             default 1\n"
              << "  --rps N                     requests/sec per connection, default 10\n"
              << "  --duration SEC              default 10\n"
              << "  --player-base ID            default 100000\n"
              << "  --ticket TICKET             default 998877\n"
              << "  --request MODULE METHOD BODY  use business request instead of ping\n"
              << "  --scene                     use PLAYER/4 enter + PLAYER/2 WalkReq\n"
              << "  --gateway-metrics FILE      gateway JSON metrics snapshot\n"
              << "  --scene-metrics FILE        scene JSON metrics snapshot\n"
              << "  --metrics-output FILE       JSONL metrics samples\n"
              << "  --metrics-interval SEC      metrics sample interval, default 1\n"
              << "  --output FILE               JSON summary, default load-summary.json\n";
}

bool ParseUInt(const char* text, uint64_t& out)
{
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    out = value;
    return true;
}

bool ParseArgs(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
            {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };
        uint64_t value = 0;
        if (arg == "--help") { Usage(argv[0]); return false; }
        if (arg == "--host") { const char* v = next("--host"); if (!v) return false; options.host = v; continue; }
        if (arg == "--output") { const char* v = next("--output"); if (!v) return false; options.output = v; continue; }
        if (arg == "--gateway-metrics") { const char* v = next("--gateway-metrics"); if (!v) return false; options.gatewayMetrics = v; continue; }
        if (arg == "--scene-metrics") { const char* v = next("--scene-metrics"); if (!v) return false; options.sceneMetrics = v; continue; }
        if (arg == "--metrics-output") { const char* v = next("--metrics-output"); if (!v) return false; options.metricsOutput = v; continue; }
        if (arg == "--scene") { options.scene = true; options.ping = false; continue; }
        if (arg == "--metrics-interval") { const char* v = next("--metrics-interval"); if (!v || !ParseUInt(v, value) || value == 0) return false; options.metricsIntervalSec = static_cast<uint32_t>(value); continue; }
        if (arg == "--port") { const char* v = next("--port"); if (!v || !ParseUInt(v, value) || value > 65535) return false; options.port = static_cast<uint16_t>(value); continue; }
        if (arg == "--connections") { const char* v = next("--connections"); if (!v || !ParseUInt(v, value) || value == 0) return false; options.connections = static_cast<uint32_t>(value); continue; }
        if (arg == "--rps") { const char* v = next("--rps"); if (!v || !ParseUInt(v, value) || value == 0) return false; options.rpsPerConnection = static_cast<uint32_t>(value); continue; }
        if (arg == "--duration") { const char* v = next("--duration"); if (!v || !ParseUInt(v, value) || value == 0) return false; options.durationSec = static_cast<uint32_t>(value); continue; }
        if (arg == "--player-base") { const char* v = next("--player-base"); if (!v || !ParseUInt(v, options.playerBase)) return false; continue; }
        if (arg == "--ticket") { const char* v = next("--ticket"); if (!v || !ParseUInt(v, options.ticket)) return false; continue; }
        if (arg == "--request")
        {
            if (i + 3 >= argc) return false;
            uint64_t module = 0, method = 0;
            if (!ParseUInt(argv[++i], module) || !ParseUInt(argv[++i], method) || module > 65535 || method > 65535) return false;
            options.module = static_cast<uint16_t>(module);
            options.method = static_cast<uint16_t>(method);
            options.body = argv[++i];
            options.ping = false;
            continue;
        }
        std::cerr << "unknown option: " << arg << "\n";
        return false;
    }
    return true;
}

int Connect(const Options& options)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return -1;
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

bool SendAll(int fd, const std::string& bytes)
{
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const ssize_t n = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (n > 0) { offset += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EINTR)) continue;
        return false;
    }
    return true;
}

std::string Encode(ClientKind kind, uint16_t module, uint16_t method, uint64_t requestId,
                   const std::string& body)
{
    ClientHeader header;
    header.kind = kind;
    header.module = module;
    header.method = method;
    header.requestId = requestId;
    Buffer buffer;
    EncodeClientFrame(buffer, header, body);
    return std::string(buffer.peek(), buffer.readableBytes());
}

std::string WalkReq(uint8_t sx, uint8_t sy, uint8_t dx, uint8_t dy)
{
    return std::string({char(0x08), char(sx), char(0x10), char(sy),
                        char(0x18), char(dx), char(0x20), char(dy)});
}

bool ReadFrame(int fd, Buffer& input, ClientHeader& header, std::string& body, int timeoutMs)
{
    for (;;)
    {
        if (DecodeClientFrame(input, header, body) == DecodeStatus::kOk)
            return true;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, timeoutMs) <= 0)
            return false;
        char bytes[8192];
        const ssize_t n = ::recv(fd, bytes, sizeof(bytes), 0);
        if (n <= 0) return false;
        input.append(bytes, static_cast<size_t>(n));
    }
}

bool Authenticate(int fd, const Options& options, uint64_t playerId, Buffer& input)
{
    if (!SendAll(fd, Encode(ClientKind::kControl, kClientSysModule, ClientSysMethod::kAuth,
                            1, MakeAuthBody(playerId, options.ticket))))
        return false;
    ClientHeader header;
    std::string body;
    if (!ReadFrame(fd, input, header, body, 3000) || header.kind != ClientKind::kControl ||
        header.method != ClientSysMethod::kAuthAck)
        return false;
    uint64_t acknowledgedPlayer = 0, sessionId = 0;
    uint32_t epoch = 0, zone = 0;
    uint8_t service = 0;
    return ParseAuthAckBody(body, acknowledgedPlayer, sessionId, epoch, zone, service) &&
           acknowledgedPlayer == playerId;
}

bool EnterScene(int fd, Buffer& input)
{
    if (!SendAll(fd, Encode(ClientKind::kRequest, 3, 4, 2, {})))
        return false;
    ClientHeader header;
    std::string body;
    if (!ReadFrame(fd, input, header, body, 3000))
        return false;
    return header.kind == ClientKind::kResponse && header.module == 3 &&
           header.method == 4 && header.requestId == 2;
}

void SampleMetrics(const Options& options, const Clock::time_point deadline)
{
    if (options.gatewayMetrics.empty() && options.sceneMetrics.empty())
        return;
    std::ofstream output;
    if (!options.metricsOutput.empty())
        output.open(options.metricsOutput, std::ios::trunc);
    while (Clock::now() < deadline)
    {
        std::ostringstream line;
        line << "{\"elapsedMs\":"
             << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - (deadline - std::chrono::seconds(options.durationSec))).count();
        const auto append = [&line](const char* prefix, const std::string& path) {
            if (path.empty()) return;
            const MetricsSnapshot sample = ReadMetricsSnapshot(path);
            for (const auto& item : sample.values)
                line << ",\"" << prefix << item.first << "\":" << item.second;
        };
        append("gateway_", options.gatewayMetrics);
        append("scene_", options.sceneMetrics);
        line << "}\n";
        if (output) output << line.str() << std::flush;
        std::cout << "metrics " << line.str();
        std::this_thread::sleep_for(std::chrono::seconds(options.metricsIntervalSec));
    }
}

void RunConnection(const Options& options, uint32_t index, LoadStats& stats,
                   const Clock::time_point deadline)
{
    const int fd = Connect(options);
    if (fd < 0) { stats.recordResult(Result::TransportError); return; }
    Buffer input;
    if (!Authenticate(fd, options, options.playerBase + index, input))
    {
        stats.recordResult(Result::TransportError);
        ::close(fd);
        return;
    }

    if (options.scene && !EnterScene(fd, input))
    {
        stats.recordResult(Result::TransportError);
        ::close(fd);
        return;
    }

    const auto interval = std::chrono::microseconds(1000000ull / options.rpsPerConnection);
    auto nextSend = Clock::now();
    // requestId=1 用于 Auth，requestId=2 用于 EnterScene；业务请求从 3 开始，
    // 避免同一会话内回程路由覆盖。
    uint64_t requestId = options.scene ? 3 : 2;
    std::map<uint64_t, Clock::time_point> pending;
    while (Clock::now() < deadline)
    {
        const auto now = Clock::now();
        while (nextSend <= now)
        {
            const uint64_t id = requestId++;
            const uint16_t module = options.ping ? kClientSysModule : (options.scene ? 3 : options.module);
            const uint16_t method = options.ping ? ClientSysMethod::kPing : (options.scene ? 2 : options.method);
            const std::string body = options.ping ? std::string() : (options.scene ? WalkReq(10, 20, 11, 20) : options.body);
            const std::string frame = Encode(options.ping ? ClientKind::kControl : ClientKind::kRequest,
                                             module, method, id, body);
            stats.recordAttempt(frame.size());
            if (!SendAll(fd, frame)) { stats.recordResult(Result::TransportError); ::close(fd); return; }
            pending.emplace(id, Clock::now());
            nextSend += interval;
            if (nextSend > now + std::chrono::seconds(1)) break;
        }

        pollfd p{fd, POLLIN, 0};
        const int waitMs = static_cast<int>(std::max<int64_t>(1,
            std::chrono::duration_cast<std::chrono::milliseconds>(nextSend - Clock::now()).count()));
        const int ready = ::poll(&p, 1, waitMs);
        if (ready <= 0) continue;
        char bytes[16384];
        const ssize_t n = ::recv(fd, bytes, sizeof(bytes), 0);
        if (n <= 0) { stats.recordResult(Result::TransportError); break; }
        input.append(bytes, static_cast<size_t>(n));
        for (;;)
        {
            ClientHeader header;
            std::string body;
            const auto status = DecodeClientFrame(input, header, body);
            if (status == DecodeStatus::kNeedMore) break;
            if (status == DecodeStatus::kError) { stats.recordResult(Result::DecodeError); break; }
            if (header.kind == ClientKind::kPush) continue;
            const auto sent = pending.find(header.requestId);
            if (sent == pending.end()) { stats.recordResult(Result::UnknownResponse); continue; }
            const auto latency = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - sent->second).count());
            Result result = Result::UnknownResponse;
            if (header.kind == ClientKind::kResponse)
                result = header.module == kClientSysModule && header.method == ClientSysMethod::kPong
                    ? Result::ControlResponse : Result::BusinessResponse;
            else if (header.kind == ClientKind::kError)
                result = header.module == kClientSysModule && header.method == ClientSysMethod::kReject
                    ? Result::GatewayReject : Result::BusinessError;
            stats.recordResult(result, latency, body.size());
            pending.erase(sent);
        }
    }
    for (size_t i = 0; i < pending.size(); ++i) stats.recordResult(Result::Timeout);
    ::close(fd);
}
}

int main(int argc, char** argv)
{
    Options options;
    if (!ParseArgs(argc, argv, options))
        return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 2;

    LoadStats stats;
    const auto deadline = Clock::now() + std::chrono::seconds(options.durationSec);
    std::thread metricsThread;
    if (!options.gatewayMetrics.empty() || !options.sceneMetrics.empty())
        metricsThread = std::thread(SampleMetrics, std::cref(options), deadline);
    std::vector<std::thread> workers;
    workers.reserve(options.connections);
    for (uint32_t i = 0; i < options.connections; ++i)
        workers.emplace_back(RunConnection, std::cref(options), i, std::ref(stats), deadline);
    for (auto& worker : workers) worker.join();
    if (metricsThread.joinable()) metricsThread.join();

    const auto result = stats.snapshot();
    std::cout << "connections=" << options.connections
              << " attempted=" << result.attempted
              << " completed=" << result.completed
              << " errors=" << result.errors
              << " timeouts=" << result.timeouts
              << " business_succeeded=" << result.businessSucceeded
              << " business_failed=" << result.businessFailed
              << " gateway_rejected=" << result.gatewayRejected
              << " transport_failed=" << result.transportFailed
              << " unknown_responses=" << result.unknownResponses
              << " decode_errors=" << result.decodeErrors
              << " p50_us=" << result.p50Us
              << " p95_us=" << result.p95Us
              << " p99_us=" << result.p99Us << '\n';
    std::FILE* file = std::fopen(options.output.c_str(), "w");
    if (file == nullptr) return 1;
    std::fprintf(file, "{\"connections\":%u,\"rps_per_connection\":%u,\"duration_sec\":%u,\"attempted\":%llu,\"completed\":%llu,\"errors\":%llu,\"timeouts\":%llu,\"business_succeeded\":%llu,\"business_failed\":%llu,\"gateway_rejected\":%llu,\"transport_failed\":%llu,\"unknown_responses\":%llu,\"decode_errors\":%llu,\"bytes_sent\":%llu,\"bytes_received\":%llu,\"min_us\":%llu,\"p50_us\":%llu,\"p95_us\":%llu,\"p99_us\":%llu,\"max_us\":%llu}\n",
                 options.connections, options.rpsPerConnection, options.durationSec,
                 (unsigned long long)result.attempted, (unsigned long long)result.completed,
                 (unsigned long long)result.errors, (unsigned long long)result.timeouts,
                 (unsigned long long)result.businessSucceeded, (unsigned long long)result.businessFailed,
                 (unsigned long long)result.gatewayRejected, (unsigned long long)result.transportFailed,
                 (unsigned long long)result.unknownResponses, (unsigned long long)result.decodeErrors,
                 (unsigned long long)result.bytesSent, (unsigned long long)result.bytesReceived,
                 (unsigned long long)result.minUs, (unsigned long long)result.p50Us,
                 (unsigned long long)result.p95Us, (unsigned long long)result.p99Us,
                 (unsigned long long)result.maxUs);
    std::fclose(file);
    return result.errors == 0 ? 0 : 1;
}