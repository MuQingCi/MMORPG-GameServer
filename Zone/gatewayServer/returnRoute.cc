#include "returnRoute.h"

bool ReturnRouteTable::Add(const ReturnRoute& r, size_t maxSize)
{
    if (r.internalSeq == 0)
        return false;

    std::lock_guard<std::mutex> lk(mtx_);
    if (routes_.size() >= maxSize)
        return false;   // 有界：宁可让这一次请求失败，也不让回程表无限增长
    routes_[r.internalSeq] = r;
    return true;
}

bool ReturnRouteTable::Take(uint64_t internalSeq, ReturnRoute& out)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = routes_.find(internalSeq);
    if (it == routes_.end())
        return false;
    out = it->second;
    routes_.erase(it);
    return true;
}

size_t ReturnRouteTable::EraseBySession(uint64_t clientSessionId)
{
    std::lock_guard<std::mutex> lk(mtx_);
    size_t n = 0;
    for (auto it = routes_.begin(); it != routes_.end();)
    {
        if (it->second.clientSessionId == clientSessionId)
        {
            it = routes_.erase(it);
            ++n;
        }
        else
        {
            ++it;
        }
    }
    return n;
}

size_t ReturnRouteTable::Sweep(int64_t nowMs, uint32_t ttlMs)
{
    std::lock_guard<std::mutex> lk(mtx_);
    size_t n = 0;
    for (auto it = routes_.begin(); it != routes_.end();)
    {
        if (nowMs - it->second.createdAtMs > static_cast<int64_t>(ttlMs))
        {
            it = routes_.erase(it);
            ++n;
        }
        else
        {
            ++it;
        }
    }
    return n;
}

size_t ReturnRouteTable::Size() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return routes_.size();
}
