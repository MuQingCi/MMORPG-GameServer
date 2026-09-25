#ifndef CLEARMOON_GATEWAY_SERVICE_REGISTRY_H
#define CLEARMOON_GATEWAY_SERVICE_REGISTRY_H

#include "common/callbacks.h"   // TcpConnectionPtr

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

/**
 * @brief 后端节点身份（**握手认证后**才绑定）
 *
 * 设计要点（与 docs/顺序实现清单.md 阶段 4 对应）：
 *   - 连接落地的第一状态是"pending"（只有 connName/连接），**没有任何身份**；
 *   - 只有收到 SYS/kHandshake 且白名单校验通过，才把 serviceId/zoneId/sceneId 绑到这条连接上；
 *   - 绑定前收到的业务帧一律丢弃（否则任何人 connect 上来就能冒充场景服）；
 *   - 同一 serviceId 只保留最新连接（后到者替换），旧连接由调用方关闭 —— 这样
 *     "场景服重启后旧连接还没被 TCP 感知为断开"时不会出现两个同号节点。
 */
struct ServiceEndpoint
{
    std::string      connName;
    uint8_t          serviceId    = 0;
    uint32_t         zoneId       = 0;
    uint32_t         sceneId      = 0;
    TcpConnectionPtr conn;
    int64_t          registeredAtMs = 0;
};

/**
 * @brief 后端服务注册表：<serviceId> -> 连接
 *
 * 线程模型：表被 baseLoop（连接建立/关闭、周期清理）与各 IO 线程（收包/路由）同时访问，
 * 因此所有公开接口内部持锁，**返回值一律是拷贝**（含 TcpConnectionPtr 拷贝：
 * 调用方拿到后即可安全地在别的线程 send()）。
 */
class ServiceRegistry
{
public:
    // 连接落地：登记为 pending（未认证）。同名连接重复 Add 视为幂等更新。
    bool AddPending(const std::string& connName, const TcpConnectionPtr& conn);

    // 握手认证：绑定节点身份。oldConnName 非空时返回"被顶掉的旧连接名"（调用方负责关闭）。
    bool Bind(const std::string& connName, uint8_t serviceId, uint32_t zoneId, uint32_t sceneId,
              int64_t nowMs, std::string* oldConnName = nullptr);

    bool GetByService(uint8_t serviceId, ServiceEndpoint& out) const;
    bool GetByName(const std::string& connName, ServiceEndpoint& out) const;

    // 连接关闭：移除该连接（含 serviceId 索引）
    bool RemoveByName(const std::string& connName, ServiceEndpoint& removed);

    size_t Size() const;        // pending + ready
    size_t ReadyCount() const;  // 已完成握手认证的节点数

private:
    mutable std::mutex mtx_;
    std::unordered_map<std::string, ServiceEndpoint> byName_;
    std::unordered_map<uint8_t, std::string> byService_;   // serviceId -> connName（单实例语义）
};

#endif
