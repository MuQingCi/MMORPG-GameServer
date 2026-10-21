#pragma once

#include <chrono>
#include <map>
#include <set>
#include <cstdint>
#include <algorithm>

// 每个连接只占一个到期项；重排时删除旧项，避免长期运行积累失效定时器。
class LoadClientSchedule
{
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    void update(uint32_t id, Time when)
    {
        erase(id);
        entries_.emplace(when, id);
        times_[id] = when;
    }
    void erase(uint32_t id)
    {
        auto it = times_.find(id);
        if (it == times_.end()) return;
        entries_.erase({it->second, id});
        times_.erase(it);
    }
    bool pop(Time now, uint32_t& id)
    {
        if (entries_.empty() || entries_.begin()->first > now) return false;
        id = entries_.begin()->second;
        erase(id);
        return true;
    }
    int waitMs(Time now) const
    {
        if (entries_.empty()) return 20;
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(entries_.begin()->first - now).count();
        return static_cast<int>(std::clamp<int64_t>((us + 999) / 1000, 0, 20));
    }
    size_t size() const { return entries_.size(); }
private:
    std::set<std::pair<Time, uint32_t>> entries_;
    std::map<uint32_t, Time> times_;
};