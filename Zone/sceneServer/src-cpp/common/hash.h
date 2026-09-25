#ifndef CLEARMOON_COMMON_HASH_H
#define CLEARMOON_COMMON_HASH_H

#include <cstddef>
#include <cstdint>

/**
 * @brief Actor / 会话的线程归属哈希
 *
 * 为什么不用 std::hash：
 *   1. std::hash 的实现随 STL 版本变化，而"同一 playerId 的消息永远落到同一逻辑线程"
 *      是 Actor 模型的正确性前提，不允许随编译环境漂移；
 *   2. playerId 若不是连续分配（例如低位编码了区服号/平台号），取模会严重倾斜，
 *      splitmix64 对这类结构化输入也能良好雪崩。
 *
 * 结论：归属算法必须由自己掌控，写成纯函数（无状态、幂等、无随机种子）。
 */
namespace hashutil
{

// splitmix64 终混合函数：稳定、雪崩性好、无依赖
inline uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/**
 * @brief 玩家 Actor 的逻辑线程归属：hash(playerId) % numWorkers
 *
 * 纯函数，不需要在进程里维护 playerId -> workerId 的表：
 *   表会泄漏、会与真实归属不一致、会让"重启后重新分片"变复杂。
 */
inline uint32_t WorkerForActor(uint64_t actorId, size_t numWorkers)
{
    if (numWorkers == 0)
        return 0;
    return static_cast<uint32_t>(SplitMix64(actorId) % numWorkers);
}

/**
 * @brief 会话（session）的兜底归属：仅用于"尚未绑定玩家"的会话消息
 *
 * 会话亲和与 Actor 亲和是两层：
 *   - 会话状态（fd/收包缓冲/认证态）可以按 session 亲和；
 *   - 角色状态必须按 playerId 亲和。
 * 因此本函数只在 playerId == 0（登录中/心跳/裸协议消息）时使用。
 */
inline uint32_t WorkerForSession(uint64_t session, size_t numWorkers)
{
    if (numWorkers == 0)
        return 0;
    // 与 actor 哈希错开常量，避免 session 与 playerId 撞到同一张分布
    return static_cast<uint32_t>(SplitMix64(session ^ 0xD1B54A32D192ED03ULL) % numWorkers);
}

}  // namespace hashutil

#endif
