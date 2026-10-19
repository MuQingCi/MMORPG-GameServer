#include "serviceRegistry.h"

#include <algorithm>

ServiceRegistry::ServiceRegistry(uint32_t poolSize)
    : connPoolSize_(std::max<uint32_t>(poolSize, 1))
{
}

bool ServiceRegistry::AddPending(const std::string& connName, const TcpConnectionPtr& conn)
{
    if (connName.empty())
        return false;
    std::lock_guard<std::mutex> lk(mtx_);
    if (byName_.find(connName) != byName_.end() || pendingConn_.find(connName) != pendingConn_.end())
        return false;
    BackendConn pending;
    pending.connName = connName;
    pending.conn = conn;
    pending.state = BackendState::Pending;
    pendingConn_.emplace(connName, std::move(pending));
    return true;
}

bool ServiceRegistry::Bind(const std::string& connName, uint8_t serviceId, uint32_t zoneId,
                           uint32_t sceneId, int64_t nowMs, std::string* oldConnName)
{
    return Bind(connName, serviceId, zoneId, sceneId, 0, 0, nowMs, oldConnName);
}

bool ServiceRegistry::Bind(const std::string& connName, uint8_t serviceId, uint32_t zoneId,
                           uint32_t sceneId, uint32_t channelId, int64_t nowMs,
                           std::string* oldConnName)
{
    return Bind(connName, serviceId, zoneId, sceneId, channelId, 0, nowMs, oldConnName);
}

bool ServiceRegistry::Bind(const std::string& connName, uint8_t serviceId, uint32_t zoneId,
                           uint32_t sceneId, uint32_t channelId, uint64_t sceneIncarnation,
                           int64_t nowMs, std::string* oldConnName)
{
    if (serviceId == 0 || channelId >= connPoolSize_)
        return false;
    std::lock_guard<std::mutex> lk(mtx_);
    auto pending = pendingConn_.find(connName);
    if (pending == pendingConn_.end())
        return false;

    const BackendKey key{serviceId, zoneId, sceneId};
    auto& pool = backendConnPools_[key];
    if (pool.channels.empty())
    {
        pool.key = key;
        pool.channels.resize(connPoolSize_);
        for (uint32_t i = 0; i < connPoolSize_; ++i)
            pool.channels[i].channelId = i;
    }

    BackendConn& slot = pool.channels[channelId];
    std::string replacedConnName;

    if (sceneIncarnation != 0 && pool.sceneIncarnation != 0 &&
        sceneIncarnation != pool.sceneIncarnation)
    {
        // 进程重启会改变整个 Scene 的进程代次；绝不能让旧代次通道与新进程的通道同时处于可选状态
        bool sameIncarnationReconnect = false;
        for (const auto& oldSlot : pool.channels)
            if (oldSlot.state == BackendState::Ready && oldSlot.connName == connName &&
                oldSlot.sceneIncarnation == sceneIncarnation)
                sameIncarnationReconnect = true;

        for (auto& oldSlot : pool.channels)
        {
            if (oldSlot.state != BackendState::Ready || oldSlot.connName.empty())
                continue;
            if (sameIncarnationReconnect && oldSlot.sceneIncarnation == sceneIncarnation)
                continue;
            auto old = byName_.find(oldSlot.connName);
            if (old != byName_.end())
            {
                if (replacedConnName.empty())
                    replacedConnName = old->first;
                old->second.serviceId = 0;
                old->second.zoneId = 0;
                old->second.sceneId = 0;
                old->second.channelId = 0;
                old->second.sceneIncarnation = 0;
            }
            const uint32_t oldChannel = oldSlot.channelId;
            const uint64_t generation = oldSlot.connectionGeneration + 1;
            oldSlot = {};
            oldSlot.channelId = oldChannel;
            oldSlot.connectionGeneration = generation;
        }
        byInstance_.erase({serviceId, zoneId, sceneId});
        auto legacy = byService_.find(serviceId);
        if (legacy != byService_.end() && byName_.find(legacy->second) != byName_.end() &&
            byName_.at(legacy->second).serviceId == 0)
            byService_.erase(legacy);
        pool.sceneIncarnation = sceneIncarnation;
    }
    if (oldConnName) oldConnName->clear();

    // 通道只在单个 Scene 池内独占。重连可以接管已占用的槽位；在调用方关闭旧 TCP 连接之前保留降级的名称记录，以保证旧连接的关闭回调仍然幂等
    if (slot.state == BackendState::Ready && slot.connName != connName)
    {
        const std::string staleName = slot.connName;
        auto stale = byName_.find(staleName);
        if (stale != byName_.end())
        {
            replacedConnName = staleName;
            stale->second.serviceId = 0;
            stale->second.zoneId = 0;
            stale->second.sceneId = 0;
            stale->second.channelId = 0;
            stale->second.sceneIncarnation = 0;
            auto legacy = byService_.find(serviceId);
            if (legacy != byService_.end() && legacy->second == staleName)
                byService_.erase(legacy);
            const ServiceInstanceKey staleKey{serviceId, zoneId, sceneId};
            auto instance = byInstance_.find(staleKey);
            if (instance != byInstance_.end() && instance->second == staleName)
                byInstance_.erase(instance);
        }
        else
            return false;
    }

    auto oldRecord = byName_.find(connName);
    if (oldRecord != byName_.end())
    {
        if (oldRecord->second.serviceId != serviceId || oldRecord->second.zoneId != zoneId ||
            oldRecord->second.sceneId != sceneId || oldRecord->second.channelId != channelId ||
            oldRecord->second.sceneIncarnation != sceneIncarnation)
            return false;
        replacedConnName = connName;
        const auto oldChannelId = oldRecord->second.channelId;
        auto oldPool = backendConnPools_.find({oldRecord->second.serviceId, oldRecord->second.zoneId,
                                               oldRecord->second.sceneId});
        if (oldPool != backendConnPools_.end() && oldChannelId < oldPool->second.channels.size())
        {
            auto& oldSlot = oldPool->second.channels[oldChannelId];
            if (oldSlot.connName == connName)
            {
                oldSlot = {};
                oldSlot.channelId = oldChannelId;
            }
        }
        oldRecord->second.serviceId = 0; // 保留失效套接字仅用于幂等的关闭回调
    }

    ServiceEndpoint endpoint;
    endpoint.connName = connName;
    endpoint.serviceId = serviceId;
    endpoint.zoneId = zoneId;
    endpoint.sceneId = sceneId;
    endpoint.channelId = channelId;
    endpoint.sceneIncarnation = sceneIncarnation;
    endpoint.conn = pending->second.conn;
    endpoint.registeredAtMs = nowMs;

    const uint64_t generation = slot.connectionGeneration + 1;
    slot.connName = connName;
    slot.conn = endpoint.conn;
    slot.state = BackendState::Ready;
    slot.sceneIncarnation = sceneIncarnation;
    slot.connectionGeneration = generation;
    pool.sceneIncarnation = sceneIncarnation;
    byName_[connName] = endpoint;
    pendingConn_.erase(pending);
    byInstance_[{serviceId, zoneId, sceneId}] = connName;
    byService_[serviceId] = connName;
    if (oldConnName && oldConnName->empty()) *oldConnName = replacedConnName;
    return true;
}

bool ServiceRegistry::GetByScene(uint8_t serviceId, uint32_t zoneId, uint32_t sceneId,
                                 ServiceEndpoint& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    const BackendKey key{serviceId, zoneId, sceneId};
    auto poolIt = backendConnPools_.find(key);
    if (poolIt == backendConnPools_.end())
        return false;
    std::vector<ServiceEndpoint> pool;
    for (const auto& channel : poolIt->second.channels)
    {
        auto it = byName_.find(channel.connName);
        if (channel.state == BackendState::Ready && it != byName_.end() &&
            (poolIt->second.sceneIncarnation == 0 ||
             channel.sceneIncarnation == poolIt->second.sceneIncarnation))
            pool.push_back(it->second);
    }
    for (const auto& endpoint : pool)
    {
        out = endpoint;
        return true;
    }
    return false;
}

std::vector<ServiceEndpoint> ServiceRegistry::GetPool(uint8_t serviceId, uint32_t zoneId,
                                                       uint32_t sceneId) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ServiceEndpoint> result;
    auto it = backendConnPools_.find({serviceId, zoneId, sceneId});
    if (it == backendConnPools_.end())
        return result;
    for (const auto& channel : it->second.channels)
    {
        auto endpoint = byName_.find(channel.connName);
        if (channel.state == BackendState::Ready && endpoint != byName_.end() &&
            (it->second.sceneIncarnation == 0 ||
             channel.sceneIncarnation == it->second.sceneIncarnation))
            result.push_back(endpoint->second);
    }
    return result;
}

bool ServiceRegistry::SelectByScene(uint8_t serviceId, uint32_t zoneId, uint32_t sceneId,
                                    ServiceEndpoint& out)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const BackendKey key{serviceId, zoneId, sceneId};
    auto it = backendConnPools_.find(key);
    if (it == backendConnPools_.end())
        return false;
    std::vector<const BackendConn*> ready;
    for (const auto& channel : it->second.channels)
        if (channel.state == BackendState::Ready && channel.conn &&
            (it->second.sceneIncarnation == 0 ||
             channel.sceneIncarnation == it->second.sceneIncarnation))
            ready.push_back(&channel);
    if (ready.empty())
        return false;
    uint64_t& next = nextRoundRobin_[key];
    const auto* selected = ready[static_cast<size_t>(next++ % ready.size())];
    auto endpoint = byName_.find(selected->connName);
    if (endpoint == byName_.end())
        return false;
    out = endpoint->second;
    return true;
}

bool ServiceRegistry::GetByChannel(const std::string& connName, ServiceEndpoint& out) const
{
    return GetByName(connName, out);
}

std::vector<ServiceEndpoint> ServiceRegistry::GetInstances(uint8_t serviceId, uint32_t zoneId) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ServiceEndpoint> result;
    for (const auto& entry : backendConnPools_)
    {
        if (entry.first.serviceId != serviceId || entry.first.zoneId != zoneId)
            continue;
        for (const auto& channel : entry.second.channels)
        {
            if (channel.state != BackendState::Ready)
                continue;
            auto endpoint = byName_.find(channel.connName);
            if (endpoint != byName_.end())
            {
                result.push_back(endpoint->second);
                break;
            }
        }
    }
    return result;
}

bool ServiceRegistry::GetByService(uint8_t serviceId, ServiceEndpoint& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = byService_.find(serviceId);
    if (it == byService_.end()) return false;
    auto endpoint = byName_.find(it->second);
    if (endpoint == byName_.end()) return false;
    out = endpoint->second;
    return true;
}

bool ServiceRegistry::GetByName(const std::string& connName, ServiceEndpoint& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = byName_.find(connName);
    if (it != byName_.end()) { out = it->second; return true; }
    auto pending = pendingConn_.find(connName);
    if (pending == pendingConn_.end()) return false;
    out = {};
    out.connName = connName;
    out.conn = pending->second.conn;
    return true;
}

bool ServiceRegistry::RemoveByName(const std::string& connName, ServiceEndpoint& removed)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto pending = pendingConn_.find(connName);
    if (pending != pendingConn_.end())
    {
        removed = {};
        removed.connName = connName;
        removed.conn = pending->second.conn;
        pendingConn_.erase(pending);
        return true;
    }
    auto it = byName_.find(connName);
    if (it == byName_.end()) return false;
    removed = it->second;
    if (removed.serviceId == 0)
    {
        byName_.erase(it);
        return true;
    }
    const BackendKey key{removed.serviceId, removed.zoneId, removed.sceneId};
    auto pool = backendConnPools_.find(key);
    if (pool != backendConnPools_.end() && removed.channelId < pool->second.channels.size())
    {
        auto& slot = pool->second.channels[removed.channelId];
        if (slot.connName == connName)
        {
            const uint64_t nextGeneration = slot.connectionGeneration + 1;
            slot = {};
            slot.channelId = removed.channelId;
            slot.connectionGeneration = nextGeneration;
            bool active = false;
            for (const auto& channel : pool->second.channels)
                if (channel.state == BackendState::Ready &&
                    channel.sceneIncarnation == pool->second.sceneIncarnation)
                    active = true;
            if (!active)
                pool->second.sceneIncarnation = 0;
        }
    }
    byName_.erase(it);
    auto instance = byInstance_.find({removed.serviceId, removed.zoneId, removed.sceneId});
    if (instance != byInstance_.end() && instance->second == connName)
    {
        instance->second.clear();
            for (const auto& entry : byName_)
                if (entry.second.serviceId != 0 && entry.second.serviceId == removed.serviceId && entry.second.zoneId == removed.zoneId &&
                entry.second.sceneId == removed.sceneId)
            { instance->second = entry.first; break; }
        if (instance->second.empty()) byInstance_.erase(instance);
    }
    auto service = byService_.find(removed.serviceId);
    if (service != byService_.end() && service->second == connName)
    {
        byService_.erase(service);
        for (const auto& entry : byName_)
            if (entry.second.serviceId != 0 && entry.second.serviceId == removed.serviceId)
            { byService_[removed.serviceId] = entry.first; break; }
    }
    return true;
}

size_t ServiceRegistry::Size() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return byName_.size() + pendingConn_.size();
}

size_t ServiceRegistry::ReadyCount() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    size_t ready = 0;
    for (const auto& entry : byName_)
        if (entry.second.serviceId != 0)
            ++ready;
    return ready;
}