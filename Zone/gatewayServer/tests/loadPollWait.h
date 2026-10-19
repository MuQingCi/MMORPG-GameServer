#ifndef CLEARMOON_LOAD_POLL_WAIT_H
#define CLEARMOON_LOAD_POLL_WAIT_H

#include <algorithm>
#include <chrono>
#include <cstdint>

// 与负载客户端共用，以便不启动压测集群即可测试等待调度。
inline int LoadPollWaitMs(std::chrono::steady_clock::time_point now,
                          std::chrono::steady_clock::time_point nextSend,
                          std::chrono::steady_clock::time_point deadline,
                          std::chrono::steady_clock::time_point drainDeadline,
                          bool stopSending, bool scenePending)
{
    // Scene 请求的响应到达前不能发送下一条请求；poll 在可读时
    // 立即唤醒；已过期的发送计划不能作为有效的等待定时器。
    // 限制所有休眠时长，确保停止信号即使发往其他线程也能被及时观察到。
    const auto wakeAt = std::min(now + std::chrono::milliseconds(100),
        stopSending ? drainDeadline : (scenePending ? deadline : std::min(nextSend, deadline)));
    return static_cast<int>(std::max<int64_t>(1,
        std::chrono::duration_cast<std::chrono::milliseconds>(wakeAt - now).count()));
}

#endif