#include "loadStats.h"
#include "loadClientReactor.h"
#include "metricsReader.h"
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <csignal>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

namespace
{
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t g_stopRequested = 0;

void HandleStopSignal(int)
{
    g_stopRequested = 1;
}

using Options = LoadClientReactorOptions;

std::atomic<int64_t> g_startEpochMs{0};
std::atomic<uint64_t> g_windowSuccessful{0};

void Usage(const char* program)
{
    std::cerr << "usage: " << program << " [options]\n"
              << "  --host HOST                 default 127.0.0.1\n"
              << "  --port PORT                 default 10000\n"
              << "  --connections N             default 1\n"
              << "  --io-threads N              epoll I/O threads, default 1\n"
              << "  --rps N                     requests/sec per connection, default 10\n"
              << "  --duration SEC              default 10\n"
              << "  --drain SEC                 stop sending, then drain responses for at most SEC\n"
              << "  --idle-before SEC           stay online before business, default 0 (max 300)\n"
              << "  --hold-after SEC            stay online after drain, default 0 (max 300)\n"
              << "  --player-base ID            default 100000\n"
              << "  --ticket TICKET             default 998877\n"
              << "  --request MODULE METHOD BODY  use business request instead of ping\n"
              << "  --scene                     use PLAYER/4 enter + PLAYER/2 WalkReq\n"
              << "  --gateway-metrics FILE      gateway JSON metrics snapshot\n"
              << "  --scene-metrics FILE        scene JSON metrics snapshot\n"
              << "  --scene-metrics-extra FILE  additional scene snapshot (repeatable)\n"
              << "  --metrics-output FILE       JSONL metrics samples\n"
              << "  --metrics-interval SEC      metrics sample interval, default 1\n"
              << "  --barrier-dir DIR           wait for START after writing READY\n"
              << "  --output FILE               JSON summary, default load-summary.json\n";
}

bool ParseUInt(const char* text, uint64_t& out)
{
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (text[0] == '-' || errno != 0 || end == text || *end != '\0')
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
        if (arg == "--barrier-dir") { const char* v = next("--barrier-dir"); if (!v) return false; options.barrierDir = v; continue; }
        if (arg == "--scene") { options.scene = true; options.ping = false; continue; }
        if (arg == "--stagger") { options.stagger = true; continue; }
        if (arg == "--warmup") { const char* v=next("--warmup"); if(!v || !ParseUInt(v,value) || value>300)return false;options.warmupSec=value;continue; }
        if (arg == "--idle-before" || arg == "--hold-after") {
            const char* v=next(arg.c_str());
            if(!v || !ParseUInt(v,value) || value>300)return false;
            (arg == "--idle-before" ? options.idleBeforeSec : options.holdAfterSec)=value;
            continue;
        }
        if (arg == "--preparation-timeout") { const char* v=next("--preparation-timeout");if(!v || !ParseUInt(v,value) || value<1 || value>600)return false;options.preparationTimeoutSec=value;continue; }
        if (arg == "--metrics-interval") { const char* v = next("--metrics-interval"); if (!v || !ParseUInt(v, value) || value == 0 || value > UINT32_MAX) return false; options.metricsIntervalSec = static_cast<uint32_t>(value); continue; }
        if (arg == "--port") { const char* v = next("--port"); if (!v || !ParseUInt(v, value) || value > 65535) return false; options.port = static_cast<uint16_t>(value); continue; }
        if (arg == "--io-threads") { const char* v = next("--io-threads"); if (!v || !ParseUInt(v, value) || value == 0 || value > UINT32_MAX) return false; options.ioThreads = static_cast<uint32_t>(value); continue; }
        if (arg == "--connections") { const char* v = next("--connections"); if (!v || !ParseUInt(v, value) || value == 0 || value > UINT32_MAX) return false; options.connections = static_cast<uint32_t>(value); continue; }
        if (arg == "--rps") { const char* v = next("--rps"); if (!v || !ParseUInt(v, value) || value == 0 || value > UINT32_MAX) return false; options.rpsPerConnection = static_cast<uint32_t>(value); continue; }
        if (arg == "--duration") { const char* v = next("--duration"); if (!v || !ParseUInt(v, value) || value == 0 || value > UINT32_MAX) return false; options.durationSec = static_cast<uint32_t>(value); continue; }
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

void SampleMetrics(const Options& options, LoadStats& stats)
{
    const bool collectMetrics = !options.gatewayMetrics.empty() || !options.sceneMetrics.empty() ||
        !options.additionalSceneMetrics.empty();
    if (!collectMetrics && options.metricsOutput.empty()) return;
    std::ofstream output;
    if (!options.metricsOutput.empty())
        output.open(options.metricsOutput, std::ios::trunc);
    if (!options.metricsOutput.empty() && !output)
    {
        g_stopRequested = 1;
        return;
    }
    while (g_startEpochMs.load() <= 0 && !g_stopRequested)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_stopRequested) return;

    const auto sampleStart = Clock::time_point(std::chrono::milliseconds(g_startEpochMs.load()))+std::chrono::seconds(options.idleBeforeSec + options.warmupSec);
    for (uint32_t sampleIndex = 0;; ++sampleIndex)
    {
        const auto nextSample = sampleStart + std::chrono::seconds(sampleIndex);
        while (Clock::now() < nextSample && !g_stopRequested)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (g_stopRequested && Clock::now() < nextSample) break;
        std::ostringstream line;
        bool metricsValid = true;
        const bool finalSample = sampleIndex >= options.durationSec + options.drainSec;
        line << "{\"elapsedMs\":" << sampleIndex * 1000
             << ",\"readMonotonicUs\":" << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()
             << ",\"finalSample\":" << (finalSample ? "true" : "false")
             << ",\"phase\":\"" << (sampleIndex <= options.durationSec ? "load" : "drain") << "\"";
        const LoadStatsSnapshot client = stats.countersSnapshot();
        line << ",\"client_attempted\":" << client.attempted
             << ",\"client_load_attempted\":" << client.loadAttempted
             << ",\"client_sent\":" << client.sent
             << ",\"client_business_succeeded\":" << client.businessSucceeded
             << ",\"client_window_succeeded\":" << g_windowSuccessful.load()
             << ",\"client_schedule_delay_us\":" << client.scheduleDelayUs
             << ",\"client_schedule_delay_max_us\":" << client.maxScheduleDelayUs
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
        if (sampleIndex >= options.durationSec + options.drainSec) break;
    }
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
    if (options.scene)
    {
        std::ofstream positions(options.output + ".players.jsonl", std::ios::trunc);
        if (!positions) { std::cerr << "cannot create player positions output\n"; return 1; }
    }
    if (!options.barrierDir.empty())
    {
        struct stat barrierStat{};
        if (::stat(options.barrierDir.c_str(), &barrierStat) != 0 || !S_ISDIR(barrierStat.st_mode))
        { std::cerr << "barrier directory must exist\n"; return 2; }
        for (uint32_t i = 0; i < options.connections; ++i)
            std::remove((options.barrierDir + "/" + options.output.substr(options.output.find_last_of('/') == std::string::npos
                ? 0 : options.output.find_last_of('/') + 1) + "." + std::to_string(i) + ".ready").c_str());
    }
    std::thread metricsThread;
    if (!options.gatewayMetrics.empty() || !options.sceneMetrics.empty() ||
        !options.additionalSceneMetrics.empty() || !options.metricsOutput.empty())
    {
        try { metricsThread = std::thread(SampleMetrics, std::cref(options), std::ref(stats)); }
        catch (const std::exception& e)
        {
            std::cerr << "cannot start metrics sampler: " << e.what() << '\n';
            return 1;
        }
    }
    const bool reactorsOk = RunLoadClientReactors(options, stats, g_startEpochMs, g_windowSuccessful, g_stopRequested);
    // 没有 START 表示准备失败；否则，即使工作线程提前排空，
    // 也应让采样器写出测量时长加排空时长的边界。
    if (g_startEpochMs.load() <= 0 || !reactorsOk) g_stopRequested = 1;
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
              << " logout_succeeded=" << result.logoutSucceeded
              << " logout_failed=" << result.logoutFailed
              << " gateway_rejected=" << result.gatewayRejected
              << " transport_failed=" << result.transportFailed
              << " unknown_responses=" << result.unknownResponses
              << " decode_errors=" << result.decodeErrors
              << " p50_us=" << result.p50Us
              << " p95_us=" << result.p95Us
              << " p99_us=" << result.p99Us << '\n';
    std::FILE* file = std::fopen(options.output.c_str(), "w");
    if (file == nullptr) return 1;
    std::fprintf(file, "{\"connections\":%u,\"rps_per_connection\":%u,\"duration_sec\":%u,\"drain_sec\":%u,\"attempted\":%llu,\"load_attempted\":%llu,\"completed\":%llu,\"in_flight\":%llu,\"drain_unfinished\":%llu,\"drain_final_unfinished\":%llu,\"cutoff_unfinished\":%llu,\"unresolved\":%llu,\"errors\":%llu,\"timeouts\":%llu,\"business_succeeded\":%llu,\"business_failed\":%llu,\"logout_succeeded\":%llu,\"logout_failed\":%llu,\"gateway_rejected\":%llu,\"transport_failed\":%llu,\"unknown_responses\":%llu,\"decode_errors\":%llu,\"bytes_sent\":%llu,\"bytes_received\":%llu,\"business_errors\":%s,",
                 options.connections, options.rpsPerConnection, options.durationSec, options.drainSec,
                 (unsigned long long)result.attempted, (unsigned long long)result.loadAttempted,
                 (unsigned long long)result.completed,
                 (unsigned long long)result.inFlight, (unsigned long long)result.drainUnfinished,
                 (unsigned long long)result.drainFinalUnfinished,
                 (unsigned long long)result.cutoffUnfinished,
                 (unsigned long long)result.unresolved,
                 (unsigned long long)result.errors, (unsigned long long)result.timeouts,
                 (unsigned long long)result.businessSucceeded, (unsigned long long)result.businessFailed,
                 (unsigned long long)result.logoutSucceeded, (unsigned long long)result.logoutFailed,
                 (unsigned long long)result.gatewayRejected, (unsigned long long)result.transportFailed,
                 (unsigned long long)result.unknownResponses, (unsigned long long)result.decodeErrors,
                 (unsigned long long)result.bytesSent, (unsigned long long)result.bytesReceived,
                 FormatBusinessErrors(result.businessErrors).c_str());
    std::fprintf(file, "\"sent\":%llu,\"schedule_delay_us\":%llu,\"schedule_delay_max_us\":%llu,",
                 (unsigned long long)result.sent, (unsigned long long)result.scheduleDelayUs,
                 (unsigned long long)result.maxScheduleDelayUs);
    std::fprintf(file, "\"p90_us\":%llu,", (unsigned long long)result.p90Us);
    std::fprintf(file, "\"min_us\":%llu,\"p50_us\":%llu,\"p95_us\":%llu,\"p99_us\":%llu,\"max_us\":%llu}\n",
                 (unsigned long long)result.minUs, (unsigned long long)result.p50Us,
                 (unsigned long long)result.p95Us, (unsigned long long)result.p99Us,
                 (unsigned long long)result.maxUs);
    std::fclose(file);
    std::ofstream latencyFile(options.output + ".latency-us.txt");
    for (const auto latency : stats.latencySamples()) latencyFile << latency << '\n';
    latencyFile.close();
    if (!latencyFile) return 1;
    return reactorsOk && result.unresolved == 0 && result.errors == 0 && (!options.scene ||
        (result.logoutSucceeded == options.connections && result.logoutFailed == 0)) ? 0 : 1;
}