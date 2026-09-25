#ifndef CLEARMOON_DB_REDISKEY_H
#define CLEARMOON_DB_REDISKEY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Redis key 命名空间（区服唯一共享 Redis 的隔离手段）
 *
 * 为什么必须有它（设计文档 3.5 / 评审 D 类）：
 *   一个区服共用一个 Redis 实例，而 key 空间是全局的。若各场景服/各业务各写各的
 *   key，就会互相覆盖：场景 1 的 `bag:1001` 与场景 2 的 `bag:1001` 会撞在一起。
 *   隔离只能靠**key 前缀**（不能用 `SELECT n`：那会让将来无法迁移到 Redis Cluster）。
 *
 * 三类 key：
 *   zone  : `zone:{zoneId}:{name}`                    —— 区服级，跨场景共享（排行榜/公会/邮件）
 *   scene : `zone:{zoneId}:scene:{sceneId}:{name}`     —— 场景级
 *   player: `zone:{zoneId}:player:{playerId}:{name}`   —— 玩家级（**不带 scene**：
 *                                                        玩家会跨场景移动，key 必须跟着人走）
 *
 * 校验规则（`Owns`）：任何 key 都必须落在本区服的 `zone:{zoneId}:` 前缀内。
 * 只要求区服前缀而不是场景前缀，是因为"场景服直接写区服级排行榜"是合法用法；
 * 不能容忍的是写到别的区服/无前缀的裸 key。
 */
struct RedisNamespace
{
    uint32_t zoneId = 0;
    uint32_t sceneId = 0;   // 0 = 只使用区服级前缀

    bool Valid() const { return zoneId != 0; }

    std::string ZonePrefix() const;    // "zone:1:"
    std::string ScenePrefix() const;   // "zone:1:scene:2:"（sceneId == 0 时退化为 ZonePrefix）

    std::string ZoneKey(const std::string& name) const;
    std::string SceneKey(const std::string& name) const;
    std::string PlayerKey(uint64_t playerId, const std::string& name) const;

    // key 是否落在本区服命名空间内
    bool Owns(const std::string& key) const;
};

/**
 * @brief Redis 命令里 key 的位置形态
 *
 * 为什么需要它：要在**发送前**校验"这条命令动的是不是我命名空间里的 key"，
 * 就必须知道每个命令的 key 在第几个参数上。Redis 没有提供机器可读的命令元数据
 * （COMMAND 可以拿，但需要先连上去、且引入额外依赖），所以这里用一张小表覆盖常用命令，
 * 未知命令按"key 在第一个参数"处理（Redis 绝大多数命令都是这个形态）。
 */
enum class RedisKeyShape : uint8_t
{
    kNoKey,           // 与 key 无关（PING/INFO/PUBLISH/PUBSUB/...）
    kSingleKey,       // args[1] 是 key
    kAllArgsAreKeys,  // args[1..] 全是 key（DEL/MGET/EXISTS/WATCH/TOUCH/PFCOUNT/...）
    kMsetPairs,       // MSET/MSETNX：key 在奇数位（1,3,5...）
    kEvalKeys,        // EVAL/EVALSHA：args[2] = numkeys，key 从 args[3] 开始
};

RedisKeyShape RedisCommandKeyShape(const std::string& command);

/**
 * @brief 按 key 稳定分片
 *
 * 关键不变量：**同一个 key 永远映射到同一个实例**，与写入顺序、进程重启无关。
 * 因此分片依据必须是 key 本身（而不是"哪个玩家发的"）：玩家会跨场景，
 * 而 `bag:1001` 这样的 key 在任意场景都必须落到同一个 Redis 实例。
 */
size_t RedisShardOf(const std::string& key, size_t shardCount);

/**
 * @brief 校验一条 Redis 命令并推导目标分片
 *
 * @param args      完整命令数组（args[0] = 命令名）
 * @param ns        本服务的 Redis 命名空间
 * @param shardCount 分片数（1 = 单实例）
 * @param shardOut  输出：该命令应发往的分片
 * @param err       失败原因（直接回给脚本）
 *
 * 失败场景：
 *   - 空命令 / key 为空串；
 *   - key 不在本区服的命名空间里（脚本裸写 key）；
 *   - 多 key 命令的 key 落在**不同分片**（Redis 单实例事务/多键操作无法跨实例完成）；
 *   - 命名空间未配置（zoneId == 0）而命令又需要 key。
 */
bool ValidateRedisCommand(const std::vector<std::string>& args,
                          const RedisNamespace& ns,
                          size_t shardCount,
                          size_t& shardOut,
                          std::string& err);

#endif
