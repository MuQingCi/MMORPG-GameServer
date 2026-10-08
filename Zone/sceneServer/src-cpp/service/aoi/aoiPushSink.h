#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIPUSHSINK_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIPUSHSINK_H

#include <string>
#include <cstdint>
#include <vector>

using workerId = uint32_t;

//Client相关定义
struct AoiClientPushRequest {
    uint64_t observerPlayerId = 0;
    uint32_t clientViewEpoch = 0;

    uint16_t module = 0;
    uint16_t method = 0;

    // 已编码的 AOI 载荷，包含 viewSeq、实体世代和必要的快照分片信息。
    std::string body;
};

enum class AoiPushResult {
    kAccepted,         // 已进入网络线程队列
    kQueueFull,        // 当前入队失败，可按原会话重试
    kObserverOffline,  // 没有有效在线绑定
    kSessionChanged,   // 当前 clientViewEpoch 与请求不匹配
    kInvalidRequest
};

//逻辑线程相关

struct AoiWorkerPublishRequest {
    uint16_t msgType = 0;
    std::vector<workerId> targets;
    std::string body;
};


struct WorkerDeliveryResult {
    workerId target;
    bool accepted;
};

struct AoiWorkerPublishResult {
    std::vector<WorkerDeliveryResult> deliveries;
};


//推送接口声明
class AoiPushSink {
public:
    virtual ~AoiPushSink() = default;

    virtual AoiPushResult sendToClient(const AoiClientPushRequest& request) = 0;

    virtual AoiWorkerPublishResult publishToWorkers(const AoiWorkerPublishRequest& request) = 0;
};


#endif