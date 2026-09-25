#ifndef CLEARMOON_GATEWAY_RETURN_ROUTE_H
#define CLEARMOON_GATEWAY_RETURN_ROUTE_H

#include <cstdint>
#include <mutex>
#include <unordered_map>

/**
 * @brief 一次"客户端请求 -> 后端响应"的回程记录
 *
 * 为什么必须有它（这是多客户端复用**一条**后端连接的唯一办法）：
 *   后端服务（场景服）在回包时只会把请求头的 `seq` 原样带回，它并不知道
 *   "这条请求来自哪个客户端连接"。网关在转发时把客户端的 requestId **替换**成
 *   网关内唯一的 internalSeq，并在这里登记映射；回包到达时按 internalSeq 反查，
 *   再把 requestId 还原成客户端自己的号。
 *
 * 由此得到两条硬性保证（也正是 docs 阶段 5 的验收点）：
 *   - 两个客户端用**相同** requestId 也不会串包（internalSeq 全局唯一）；
 *   - 顶号/重连后旧会话的迟到回包不会投给新会话（sessionEpoch 校验 + 会话销毁时清路由）。
 */
struct ReturnRoute
{
    uint64_t internalSeq     = 0;
    uint64_t clientSessionId = 0;
    uint64_t clientRequestId = 0;
    uint32_t sessionEpoch    = 0;
    int64_t  createdAtMs     = 0;
};

/**
 * @brief 回程路由表：internalSeq -> 回程记录
 *
 * 两个方向都是"有界"的：
 *   - 容量上限（maxSize，由配置给出）：满了直接拒绝新请求并回错误，而不是无限增长；
 *   - TTL 清理：后端服务不响应（或响应丢失）时，记录必须能被回收。
 */
class ReturnRouteTable
{
public:
    bool Add(const ReturnRoute& r, size_t maxSize);
    bool Take(uint64_t internalSeq, ReturnRoute& out);

    size_t EraseBySession(uint64_t clientSessionId);
    size_t Sweep(int64_t nowMs, uint32_t ttlMs);

    size_t Size() const;

private:
    mutable std::mutex mtx_;
    std::unordered_map<uint64_t, ReturnRoute> routes_;
};

#endif
