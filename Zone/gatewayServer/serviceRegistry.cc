#include "serviceRegistry.h"

#include <utility>

bool ServiceRegistry::AddPending(const std::string& connName, const TcpConnectionPtr& conn)
{
    if (connName.empty())
        return false;

    std::lock_guard<std::mutex> lk(mtx_);

    ServiceEndpoint& ep = byName_[connName];
    // 幂等：同名连接的重复登记只更新连接对象，不重置已认证身份
    ep.connName = connName;
    ep.conn = conn;
    return true;
}

bool ServiceRegistry::Bind(const std::string& connName, uint8_t serviceId, uint32_t zoneId,
                           uint32_t sceneId, int64_t nowMs, std::string* oldConnName)
{
    if (serviceId == 0)
        return false;

    std::lock_guard<std::mutex> lk(mtx_);

    auto it = byName_.find(connName);
    if (it == byName_.end())
        return false;   // 不是本网关 accept 出来的连接：不接受注册

    // 同一连接重新握手时，先摘掉旧的实例索引，避免一个连接同时出现在两个 sceneId。
    if (it->second.serviceId != 0)
    {
        byInstance_.erase({it->second.serviceId, it->second.zoneId, it->second.sceneId});
        auto oldService = byService_.find(it->second.serviceId);
        if (oldService != byService_.end() && oldService->second == connName)
            byService_.erase(oldService);
    }

    const ServiceInstanceKey key{serviceId, zoneId, sceneId};
    // 同一实例只保留最新连接；不同 sceneId 可以并存。
    auto instanceIt = byInstance_.find(key);
    if (instanceIt != byInstance_.end() && instanceIt->second != connName)
    {
        if (oldConnName != nullptr)
            *oldConnName = instanceIt->second;
        // 旧连接的记录降级为 pending（清身份），随后由 RemoveByName 真正删除
        auto oit = byName_.find(instanceIt->second);
        if (oit != byName_.end())
        {
            oit->second.serviceId = 0;
            oit->second.zoneId = 0;
            oit->second.sceneId = 0;
        }
        byInstance_.erase(instanceIt);
    }

    it->second.serviceId = serviceId;
    it->second.zoneId = zoneId;
    it->second.sceneId = sceneId;
    it->second.registeredAtMs = nowMs;
    byInstance_[key] = connName;
    byService_[serviceId] = connName;
    return true;
}

bool ServiceRegistry::GetByScene(uint8_t serviceId, uint32_t zoneId, uint32_t sceneId,
                                 ServiceEndpoint& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    const auto instance = byInstance_.find({serviceId, zoneId, sceneId});
    if (instance == byInstance_.end())
        return false;
    const auto endpoint = byName_.find(instance->second);
    if (endpoint == byName_.end())
        return false;
    out = endpoint->second;
    return true;
}

std::vector<ServiceEndpoint> ServiceRegistry::GetInstances(uint8_t serviceId, uint32_t zoneId) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ServiceEndpoint> result;
    for (const auto& [key, connName] : byInstance_)
    {
        if (key.serviceId != serviceId || key.zoneId != zoneId)
            continue;
        const auto endpoint = byName_.find(connName);
        if (endpoint != byName_.end())
            result.push_back(endpoint->second);
    }
    return result;
}

bool ServiceRegistry::GetByService(uint8_t serviceId, ServiceEndpoint& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto sit = byService_.find(serviceId);
    if (sit == byService_.end())
        return false;
    auto it = byName_.find(sit->second);
    if (it == byName_.end())
        return false;
    out = it->second;
    return true;
}

bool ServiceRegistry::GetByName(const std::string& connName, ServiceEndpoint& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = byName_.find(connName);
    if (it == byName_.end())
        return false;
    out = it->second;
    return true;
}

bool ServiceRegistry::RemoveByName(const std::string& connName, ServiceEndpoint& removed)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = byName_.find(connName);
    if (it == byName_.end())
        return false;

    removed = std::move(it->second);
    byName_.erase(it);

    if (removed.serviceId != 0)
    {
        byInstance_.erase({removed.serviceId, removed.zoneId, removed.sceneId});
        auto sit = byService_.find(removed.serviceId);
        // 只有当索引仍指向这条连接时才摘除（否则会误摘新连接）
        if (sit != byService_.end() && sit->second == connName)
            byService_.erase(sit);
    }
    return true;
}

size_t ServiceRegistry::Size() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return byName_.size();
}

size_t ServiceRegistry::ReadyCount() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return byInstance_.size();
}
