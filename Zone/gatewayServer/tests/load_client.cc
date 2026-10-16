#include "loadStats.h"
#include "loadClientProtocol.h"
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
#include <algorithm>
#include <fstream>
#include <sstream>
#include <mutex>
#include <poll.h>
#include <csignal>
#include <string>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t g_stopRequested = 0;

void HandleStopSignal(int)
{
    g_stopRequested = 1;
}

struct Options
{
    std::string host = "127.0.0.1";
    uint16_t port = 10000;
    uint32_t connections = 1;
    uint32_t rpsPerConnection = 10;
    uint32_t durationSec = 10;
    uint32_t drainSec = 0;
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
    std::vector<std::string> additionalSceneMetrics;
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
              << "  --drain SEC                 stop sending, then drain responses for at most SEC\n"
              << "  --player-base ID            default 100000\n"
              << "  --ticket TICKET             default 998877\n"
              << "  --request MODULE METHOD BODY  use business request instead of ping\n"
              << "  --scene                     use PLAYER/4 enter + PLAYER/2 WalkReq\n"
              << "  --gateway-metrics FILE      gateway JSON metrics snapshot\n"
              << "  --scene-metrics FILE        scene JSON metrics snapshot\n"
              << "  --scene-metrics-extra FILE  additional scene snapshot (repeatable)\n"
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
        if (arg == "--scene-metrics-extra") { const char* v = next("--scene-metrics-extra"); if (!v) return false; options.additionalSceneMetrics.emplace_back(v); continue; }
        if (arg == "--metrics-output") { const char* v = next("--metrics-output"); if (!v) return false; options.metricsOutput = v; continue; }
        if (arg == "--scene") { options.scene = true; options.ping = false; continue; }
        if (arg == "--metrics-interval") { const char* v = next("--metrics-interval"); if (!v || !ParseUInt(v, value) || value == 0) return false; options.metricsIntervalSec = static_cast<uint32_t>(value); continue; }
        if (arg == "--port") { const char* v = next("--port"); if (!v || !ParseUInt(v, value) || value > 65535) return false; options.port = static_cast<uint16_t>(value); continue; }
        if (arg == "--connections") { const char* v = next("--connections"); if (!v || !ParseUInt(v, value) || value == 0) return false; options.connections = static_cast<uint32_t>(value); continue; }
        if (arg == "--rps") { const char* v = next("--rps"); if (!v || !ParseUInt(v, value) || value == 0) return false; options.rpsPerConnection = static_cast<uint32_t>(value); continue; }
        if (arg == "--duration") { const char* v = next("--duration"); if (!v || !ParseUInt(v, value) || value == 0) return false; options.durationSec = static_cast<uint32_t>(value); continue; }
        if (arg == "--drain") { const char* v = next("--drain"); if (!v || !ParseUInt(v, value) || value > 300) return false; options.drainSec = static_cast<uint32_t>(value); continue; }
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

std::string WalkReq(int32_t sx, int32_t sy, int32_t dx, int32_t dy)
{
    gs::WalkReq req;
    req.set_sx(sx);
    req.set_sy(sy);
    req.set_dx(dx);
    req.set_dy(dy);
    return req.SerializeAsString();
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (unsigned char c : value)
    {
        switch (c)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u00" << "0123456789abcdef"[c >> 4] << "0123456789abcdef"[c & 0x0f];
                else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string FormatBusinessErrors(const std::map<std::string, uint64_t>& errors)
{
    std::ostringstream out;
    out << '{';
    bool first = true;
    for (const auto& item : errors)
    {
        if (!first) out << ',';
        first = false;
        out << '"' << JsonEscape(item.first) << "\":" << item.second;
    }
    out << '}';
    return out.str();
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

bool EnterScene(int fd, Buffer& input, int32_t& x, int32_t& y)
{
    if (!SendAll(fd, Encode(ClientKind::kRequest, 3, 4, 2, {})))
        return false;
    for (;;)
    {
        ClientHeader header;
        std::string body;
        if (!ReadFrame(fd, input, header, body, 3000)) return false;
        if (header.kind == ClientKind::kPush) { ApplySceneStatePush(header, body, x, y); continue; }
        if (!MatchesLoadRequestResponse(header, 2, 3, 4)) return false;
        if (ClassifyLoadResponse(header, body, 3, 4) != LoadResponseKind::Success) return false;
        return AdvanceEnterScenePosition(body, x, y);
    }
}

void SampleMetrics(const Options& options, LoadStats& stats, const Clock::time_point start)
{
    if (options.gatewayMetrics.empty() && options.sceneMetrics.empty() &&
        options.additionalSceneMetrics.empty())
        return;
    std::ofstream output;
    if (!options.metricsOutput.empty())
        output.open(options.metricsOutput, std::ios::trunc);
    uint32_t sampleIndex = 0;
    for (;; ++sampleIndex)
    {
        const auto nextSample = start + std::chrono::seconds(sampleIndex);
        while (Clock::now() < nextSample && !g_stopRequested)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (g_stopRequested) break;
        if (sampleIndex > options.durationSec + options.drainSec &&
            stats.snapshot().inFlight == 0 && !g_stopRequested)
            break;
        std::ostringstream line;
        bool metricsValid = true;
        const bool finalSample = sampleIndex >= options.durationSec + options.drainSec;
        line << "{\"elapsedMs\":" << sampleIndex * 1000
             << ",\"finalSample\":" << (finalSample ? "true" : "false")
             << ",\"phase\":\"" << (sampleIndex <= options.durationSec ? "load" : "drain") << "\"";
        const LoadStatsSnapshot client = stats.snapshot();
        line << ",\"client_attempted\":" << client.attempted
             << ",\"client_load_attempted\":" << client.loadAttempted
             << ",\"client_completed\":" << client.completed
             << ",\"client_in_flight\":" << client.inFlight
             << ",\"client_cutoff_unfinished\":" << client.cutoffUnfinished
             << ",\"client_errors\":" << client.errors
             << ",\"client_business_errors\":" << FormatBusinessErrors(client.businessErrors)
             << ",\"client_business_failed\":" << client.businessFailed
             << ",\"client_gateway_rejected\":" << client.gatewayRejected
             << ",\"client_unknown_responses\":" << client.unknownResponses;
        const auto append = [&line, &metricsValid](const std::string& prefix, const std::string& path) {
            if (path.empty()) { metricsValid = false; return; }
            const MetricsSnapshot sample = ReadMetricsSnapshot(path);
            if (!sample.valid) metricsValid = false;
            for (const auto& item : sample.values)
                line << ",\"" << prefix << item.first << "\":" << item.second;
        };
        if (!options.gatewayMetrics.empty()) append("gateway_", options.gatewayMetrics);
        if (!options.sceneMetrics.empty()) append("scene0_", options.sceneMetrics);
        for (size_t i = 0; i < options.additionalSceneMetrics.size(); ++i)
            append("scene" + std::to_string(i + 1) + "_", options.additionalSceneMetrics[i]);
        line << ",\"metricsValid\":" << (metricsValid ? "true" : "false");
        line << "}\n";
        if (output) output << line.str() << std::flush;
        std::cout << "metrics " << line.str();
        if (stats.snapshot().inFlight == 0 && sampleIndex >= options.durationSec + options.drainSec) break;
    }
}

void RunConnection(const Options& options, uint32_t index, LoadStats& stats,
                   const Clock::time_point start)
{
    const int fd = Connect(options);
    if (fd < 0) { stats.recordResult(Result::TransportError); stats.recordUnresolved(1); return; }
    Buffer input;
    if (!Authenticate(fd, options, options.playerBase + index, input))
    {
        stats.recordUnresolved(1);
        stats.recordResult(Result::TransportError);
        ::close(fd);
        return;
    }

    int32_t x = 0, y = 0;
    if (options.scene && !EnterScene(fd, input, x, y))
    {
        stats.recordUnresolved(1);
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
    std::map<uint64_t, std::string> pendingBodies;
    bool stopSending = false;
    auto deadline = start + std::chrono::seconds(options.durationSec);
    auto drainDeadline = deadline + std::chrono::seconds(options.drainSec);
    uint64_t drainPeak = 0;
    bool cutoffRecorded = false;
    bool drainFinished = false;
    while (true)
    {
        auto now = Clock::now();
        if (g_stopRequested && !stopSending)
            deadline = now;
        if (!stopSending && (now >= deadline || g_stopRequested))
        {
            stopSending = true;
            const uint64_t atCutoff = pending.size();
            stats.recordCutoffUnfinished(atCutoff);
            cutoffRecorded = true;
            if (g_stopRequested) deadline = now;
            drainDeadline = std::min(drainDeadline, now + std::chrono::seconds(options.drainSec));
            drainPeak = atCutoff;
        }
        if (stopSending && pending.empty()) { drainFinished = true; break; }
        if (stopSending && options.drainSec == 0) break;
        if (stopSending && now >= drainDeadline)
        {
            if (!pending.empty()) stats.recordUnresolved(pending.size());
            stats.recordInFlightDelta(-static_cast<int64_t>(pending.size()));
            pending.clear();
            pendingBodies.clear();
            break;
        }
        if (stopSending) drainPeak = std::max<uint64_t>(drainPeak, pending.size());
        while (!stopSending && nextSend <= now && !(options.scene && !pending.empty()))
        {
            const uint64_t id = requestId++;
            const uint16_t module = options.ping ? kClientSysModule : (options.scene ? 3 : options.module);
            const uint16_t method = options.ping ? ClientSysMethod::kPing : (options.scene ? 2 : options.method);
            const std::string body = options.ping ? std::string() :
                (options.scene ? WalkReq(x, y, x + 1, y) : options.body);
            if (options.scene && !options.ping) pendingBodies[id] = body;
            const std::string frame = Encode(options.ping ? ClientKind::kControl : ClientKind::kRequest,
                                             module, method, id, body);
            stats.recordAttempt(frame.size());
            pending.emplace(id, Clock::now());
            stats.recordInFlightDelta(1);
            if (!SendAll(fd, frame))
            {
                stats.recordResult(Result::TransportError);
                stats.recordUnresolved(pending.size());
                stats.recordInFlightDelta(-static_cast<int64_t>(pending.size()));
                pending.clear();
                pendingBodies.clear();
                ::close(fd);
                return;
            }
            nextSend += interval;
            if (nextSend > now + std::chrono::seconds(1)) break;
        }

        pollfd p{fd, POLLIN, 0};
        const auto wakeAt = stopSending ? drainDeadline : std::min(nextSend, deadline);
        const int waitMs = static_cast<int>(std::max<int64_t>(1,
            std::chrono::duration_cast<std::chrono::milliseconds>(wakeAt - Clock::now()).count()));
        const int ready = ::poll(&p, 1, waitMs);
        if (ready < 0 && errno == EINTR)
        {
            if (g_stopRequested)
            {
                deadline = Clock::now();
                continue;
            }
            stats.recordResult(Result::TransportError);
            stats.recordInFlightDelta(-static_cast<int64_t>(pending.size()));
            pending.clear();
            pendingBodies.clear();
            break;
        }
        if (ready == 0)
        {
            if (g_stopRequested) deadline = Clock::now();
            continue;
        }
        char bytes[16384];
        const ssize_t n = ::recv(fd, bytes, sizeof(bytes), 0);
        if (n <= 0)
        {
            stats.recordInFlightDelta(-static_cast<int64_t>(pending.size()));
            pending.clear();
            pendingBodies.clear();
            if (n < 0) stats.recordResult(Result::TransportError);
            else stopSending = true;
            break;
        }
        input.append(bytes, static_cast<size_t>(n));
        for (;;)
        {
            ClientHeader header;
            std::string body;
            const auto status = DecodeClientFrame(input, header, body);
            if (status == DecodeStatus::kNeedMore) break;
            if (status == DecodeStatus::kError)
            {
                stats.recordResult(Result::DecodeError);
                stats.recordInFlightDelta(-static_cast<int64_t>(pending.size()));
                pending.clear();
                pendingBodies.clear();
                stopSending = true;
                break;
            }
            if (header.kind == ClientKind::kPush)
            {
                if (options.scene) ApplySceneStatePush(header, body, x, y);
                continue;
            }
            const auto sent = pending.find(header.requestId);
            if (sent == pending.end()) { stats.recordResult(Result::UnknownResponse); continue; }
            const uint16_t expectedModule = options.ping ? kClientSysModule : (options.scene ? 3 : options.module);
            const uint16_t expectedMethod = options.ping ? ClientSysMethod::kPong : (options.scene ? 2 : options.method);
            if (!MatchesLoadRequestResponse(header, sent->first, expectedModule, expectedMethod))
            {
                stats.recordResult(Result::UnknownResponse);
                pending.erase(sent);
                pendingBodies.erase(header.requestId);
                stats.recordInFlightDelta(-1);
                continue;
            }
            const auto latency = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - sent->second).count());
            const LoadResponseKind classified = ClassifyLoadResponse(header, body, expectedModule, expectedMethod);
            std::string businessError;
            if (options.scene && !options.ping && classified == LoadResponseKind::BusinessError)
            {
                gs::RetTip tip;
                if (IsRetTipBody(body) && tip.ParseFromString(body))
                    businessError = std::to_string(tip.code()) + ":" + tip.text();
                else if (options.scene && expectedMethod == 2)
                {
                    gs::WalkAck ack;
                    if (ack.ParseFromString(body) && ack.code() != 0)
                        businessError = std::to_string(ack.code()) + ":WalkAck";
                }
                stats.recordBusinessError(businessError.empty() ? "unclassified" : businessError);
            }
            Result result = Result::UnknownResponse;
            if (classified == LoadResponseKind::Success)
                result = options.ping ? Result::ControlResponse : Result::BusinessResponse;
            else if (classified == LoadResponseKind::GatewayReject) result = Result::GatewayReject;
            else if (classified == LoadResponseKind::BusinessError) result = Result::BusinessError;
            else
            {
                stats.recordResult(Result::DecodeError);
                pending.erase(sent);
                pendingBodies.erase(header.requestId);
                stats.recordInFlightDelta(-1);
                continue;
            }
            stats.recordResult(result, latency, body.size());
            if (options.scene && classified == LoadResponseKind::Success && expectedMethod == 2)
            {
                const auto request = pendingBodies.find(header.requestId);
                gs::WalkReq walk;
                if (request != pendingBodies.end() && walk.ParseFromString(request->second) &&
                    walk.sx() == x && walk.sy() == y)
                    AdvanceWalkPosition(body, x, y);
            }
            pending.erase(sent);
            pendingBodies.erase(header.requestId);
            stats.recordInFlightDelta(-1);
        }
    }
    if (!stopSending && !pending.empty())
    {
        stats.recordCutoffUnfinished(pending.size());
        cutoffRecorded = true;
    }
    if (stopSending && options.drainSec > 0)
    {
        if (pending.size() > drainPeak)
            stats.recordDrainUnfinished(pending.size() - drainPeak);
        stats.recordDrainFinalUnfinished(pending.size());
    }
    if (!cutoffRecorded && !pending.empty()) stats.recordCutoffUnfinished(pending.size());
    if (!pending.empty()) stats.recordUnresolved(pending.size());
    if (drainFinished) stats.recordDrainFinalUnfinished(0);
    for (size_t i = 0; i < pending.size(); ++i)
        stats.recordInFlightDelta(-1);
    ::close(fd);
}
}

int main(int argc, char** argv)
{
    Options options;
    if (!ParseArgs(argc, argv, options))
        return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 2;

    struct sigaction stopAction{};
    stopAction.sa_handler = HandleStopSignal;
    sigemptyset(&stopAction.sa_mask);
    sigaction(SIGTERM, &stopAction, nullptr);
    sigaction(SIGINT, &stopAction, nullptr);

    if (!options.metricsOutput.empty())
    {
        const size_t slash = options.metricsOutput.find_last_of('/');
        if (slash != std::string::npos && slash != 0)
        {
            const std::string directory = options.metricsOutput.substr(0, slash);
            if (::mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST)
            { std::cerr << "cannot create metrics output directory\n"; return 1; }
        }
    }
    {
        const size_t slash = options.output.find_last_of('/');
        if (slash != std::string::npos && slash != 0)
        {
            const std::string directory = options.output.substr(0, slash);
            if (::mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST)
            { std::cerr << "cannot create summary output directory\n"; return 1; }
        }
    }

    LoadStats stats;
    const auto start = Clock::now();
    std::vector<std::thread> workers;
    workers.reserve(options.connections);
    for (uint32_t i = 0; i < options.connections; ++i)
        workers.emplace_back(RunConnection, std::cref(options), i, std::ref(stats), start);
    std::thread metricsThread;
    if (!options.gatewayMetrics.empty() || !options.sceneMetrics.empty() ||
        !options.additionalSceneMetrics.empty())
        metricsThread = std::thread(SampleMetrics, std::cref(options), std::ref(stats), start);
    for (auto& worker : workers) worker.join();
    if (metricsThread.joinable()) metricsThread.join();

    const auto result = stats.snapshot();
    std::cout << "connections=" << options.connections
              << " attempted=" << result.attempted
              << " completed=" << result.completed
              << " in_flight=" << result.inFlight
              << " drain_unfinished=" << result.drainUnfinished
              << " cutoff_unfinished=" << result.cutoffUnfinished
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
    std::fprintf(file, "{\"connections\":%u,\"rps_per_connection\":%u,\"duration_sec\":%u,\"drain_sec\":%u,\"attempted\":%llu,\"load_attempted\":%llu,\"completed\":%llu,\"in_flight\":%llu,\"drain_unfinished\":%llu,\"drain_final_unfinished\":%llu,\"cutoff_unfinished\":%llu,\"unresolved\":%llu,\"errors\":%llu,\"timeouts\":%llu,\"business_succeeded\":%llu,\"business_failed\":%llu,\"gateway_rejected\":%llu,\"transport_failed\":%llu,\"unknown_responses\":%llu,\"decode_errors\":%llu,\"bytes_sent\":%llu,\"bytes_received\":%llu,\"business_errors\":%s,",
                 options.connections, options.rpsPerConnection, options.durationSec, options.drainSec,
                 (unsigned long long)result.attempted, (unsigned long long)result.loadAttempted,
                 (unsigned long long)result.completed,
                 (unsigned long long)result.inFlight, (unsigned long long)result.drainUnfinished,
                 (unsigned long long)result.drainFinalUnfinished,
                 (unsigned long long)result.cutoffUnfinished,
                 (unsigned long long)result.unresolved,
                 (unsigned long long)result.errors, (unsigned long long)result.timeouts,
                 (unsigned long long)result.businessSucceeded, (unsigned long long)result.businessFailed,
                 (unsigned long long)result.gatewayRejected, (unsigned long long)result.transportFailed,
                 (unsigned long long)result.unknownResponses, (unsigned long long)result.decodeErrors,
                 (unsigned long long)result.bytesSent, (unsigned long long)result.bytesReceived,
                 FormatBusinessErrors(result.businessErrors).c_str());
    std::fprintf(file, "\"min_us\":%llu,\"p50_us\":%llu,\"p95_us\":%llu,\"p99_us\":%llu,\"max_us\":%llu}\n",
                 (unsigned long long)result.minUs, (unsigned long long)result.p50Us,
                 (unsigned long long)result.p95Us, (unsigned long long)result.p99Us,
                 (unsigned long long)result.maxUs);
    std::fclose(file);
    return result.errors == 0 ? 0 : 1;
}