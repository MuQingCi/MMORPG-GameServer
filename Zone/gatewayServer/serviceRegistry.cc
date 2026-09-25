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

    // 同一 serviceId 只留最新连接：旧连接交给调用方关闭
    auto sit = byService_.find(serviceId);
    if (sit != byService_.end() && sit->second != connName)
    {
        if (oldConnName != nullptr)
            *oldConnName = sit->second;
        // 旧连接的记录降级为 pending（清身份），随后由 RemoveByName 真正删除
        auto oit = byName_.find(sit->second);
        if (oit != byName_.end())
        {
            oit->second.serviceId = 0;
            oit->second.zoneId = 0;
            oit->second.sceneId = 0;
        }
    }

    it->second.serviceId = serviceId;
    it->second.zoneId = zoneId;
    it->second.sceneId = sceneId;
    it->second.registeredAtMs = nowMs;
    byService_[serviceId] = connName;
    return true;
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
    return byService_.size();
}
