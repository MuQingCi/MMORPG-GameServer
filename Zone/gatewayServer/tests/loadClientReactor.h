#pragma once

#include "loadStats.h"
#include <atomic>
#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

struct LoadClientReactorOptions
{
    std::string host = "127.0.0.1";
    uint16_t port = 10000;
    uint32_t connections = 1;
    uint32_t ioThreads = 1;
    uint32_t rpsPerConnection = 10;
    uint32_t durationSec = 10;
    uint32_t drainSec = 0;
    uint32_t warmupSec = 0;
    uint32_t idleBeforeSec = 0;
    uint32_t holdAfterSec = 0;
    uint32_t preparationTimeoutSec = 30;
    bool stagger = false;
    uint64_t playerBase = 100000;
    uint64_t ticket = 998877;
    uint16_t module = 1;
    uint16_t method = 6;
    std::string body;
    std::string output = "load-summary.json";
    bool ping = true;
    bool scene = false;
    std::string gatewayMetrics;
    std::string sceneMetrics;
    std::vector<std::string> additionalSceneMetrics;
    std::string metricsOutput;
    uint32_t metricsIntervalSec = 1;
    std::string barrierDir;
};

// 所有连接只归所属 reactor 写入；调用返回前关闭 socket 并 join 全部 I/O 线程。
bool RunLoadClientReactors(const LoadClientReactorOptions& options, LoadStats& stats,
                          std::atomic<int64_t>& startEpochMs,
                          std::atomic<uint64_t>& windowSuccessful,
                          const volatile std::sig_atomic_t& stopRequested);