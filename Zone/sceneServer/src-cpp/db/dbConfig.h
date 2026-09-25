#ifndef CLEARMOON_DB_DBCONFIG_H
#define CLEARMOON_DB_DBCONFIG_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

/**
 * @brief 数据层配置
 *
 * 部署约束（设计文档 3.5）：
 *   - MySQL：区服整体唯一且共享；**区服内不做读写分离**（方案 A）——
 *     游戏里 99% 的读是"刚写过的数据马上要读"，读写分离最不适用；
 *     从库只用于备份/离线分析。因此这里只需要一个主库地址。
 *   - Redis：区服唯一共享 + **key 前缀隔离**（`zone:{zoneId}:scene:{sceneId}:...`）。
 *     可选多实例：用 `shards` 配多个端点做**逻辑分片**（同一 key 恒等落到同一实例）。
 */
struct MySqlConfig
{
    std::string host = "127.0.0.1";
    uint16_t    port = 3306;
    std::string user = "root";
    std::string password;
    std::string dbName;
    std::string charset = "utf8mb4";

    uint32_t connectTimeoutSec = 3;
    uint32_t readTimeoutSec    = 5;
    uint32_t writeTimeoutSec   = 5;
};

/**
 * @brief Redis 连接与命名空间配置
 *
 * 命名空间的权威来源：
 *   - zoneId  **只来自 ZoneConfig.zoneId**（ZoneConfig::Load 会把它填进来），
 *     避免同一个值在两个文件里各写一份（迟早不一致）；
 *   - sceneId 0 表示"区服级 key"（跨场景共享，如排行榜/公会）；
 *     场景服启动时会用 SceneConfig.sceneId 填充，见 SceneConfig::AttachZone()。
 *
 * 为什么删掉 dbIndex：用 `SELECT n` 切数据库做隔离会让将来**无法迁移到 Redis Cluster**。
 * 隔离一律用 key 前缀；字段删掉而不是留着不用，避免后来者以为它有效。
 */
struct RedisConfig
{
    std::string host = "127.0.0.1";
    uint16_t    port = 6379;
    std::string password;
    uint32_t    timeoutMs = 1000;

    uint32_t    zoneId  = 0;    // 区服ID（由 ZoneConfig.zoneId 派生，不单独配）
    uint32_t    sceneId = 0;    // 场景服ID；0 = 区服级 Key

    // 可选：多实例逻辑分片端点列表（"ip:port"）。为空 = 单实例，使用上面的 host/port。
    // 分片规则见 db/redisKey.h::RedisShardOf：按 **key** 稳定哈希选实例，
    // 从而保证"同一个 key 永远落在同一个实例"，与命令到达顺序无关。
    std::vector<std::string> shards;

    size_t ShardCount() const { return shards.empty() ? 1u : shards.size(); }

    /**
     * @brief 取第 shard 个分片的端点
     * @return false 表示 shards 里的写法非法（不是 ip:port）
     */
    bool EndpointOf(size_t shard, std::string& outHost, uint16_t& outPort) const
    {
        if (shards.empty())
        {
            if (shard != 0)
                return false;
            outHost = host;
            outPort = port;
            return true;
        }

        if (shard >= shards.size())
            return false;

        const std::string& spec = shards[shard];
        const size_t colon = spec.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size())
            return false;

        const long p = std::strtol(spec.c_str() + colon + 1, nullptr, 10);
        if (p <= 0 || p > 65535)
            return false;

        outHost = spec.substr(0, colon);
        outPort = static_cast<uint16_t>(p);
        return true;
    }
};

struct DbConfig
{
    MySqlConfig mysql;
    RedisConfig redis;
};

#endif

