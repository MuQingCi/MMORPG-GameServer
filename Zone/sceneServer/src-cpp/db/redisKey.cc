#include "db/redisKey.h"

#include "common/hash.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
// 字符串稳定哈希：FNV-1a 64 + splitmix64 终混合。
// 不用 std::hash：它的实现随 STL 变化，而"同一 key 永远同一分片"是正确性前提。
uint64_t HashKey(const std::string& s)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (unsigned char c : s)
    {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001B3ULL;
    }
    return hashutil::SplitMix64(h);
}

std::string ToUpper(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// 与 key 无关的命令：命令名本身不指向任何 key
bool IsNoKeyCommand(const std::string& cmd)
{
    static const char* kCmds[] = {
        "PING", "ECHO", "INFO", "TIME", "DBSIZE", "FLUSHALL", "FLUSHDB", "SELECT",
        "AUTH", "CLIENT", "COMMAND", "CONFIG", "LASTSAVE", "SAVE", "BGSAVE",
        "ROLE", "WAIT", "RANDOMKEY", "UNWATCH", "DISCARD", "MULTI", "EXEC",
        // 频道不是 key：频道命名规范另行约定
        "PUBLISH", "PUBSUB", "SUBSCRIBE", "UNSUBSCRIBE", "PSUBSCRIBE", "PUNSUBSCRIBE",
    };
    for (const char* c : kCmds)
    {
        if (cmd == c)
            return true;
    }
    return false;
}

// args[1..] 全是 key 的命令
bool IsAllArgsKeysCommand(const std::string& cmd)
{
    static const char* kCmds[] = {
        "DEL", "UNLINK", "EXISTS", "MGET", "WATCH", "TOUCH", "PFCOUNT",
        "SDIFF", "SINTER", "SUNION",
    };
    for (const char* c : kCmds)
    {
        if (cmd == c)
            return true;
    }
    return false;
}

bool IsMsetCommand(const std::string& cmd)
{
    return cmd == "MSET" || cmd == "MSETNX";
}

bool IsEvalCommand(const std::string& cmd)
{
    return cmd == "EVAL" || cmd == "EVALSHA" || cmd == "EVAL_RO" || cmd == "EVALSHA_RO";
}
}  // namespace

std::string RedisNamespace::ZonePrefix() const
{
    return "zone:" + std::to_string(zoneId) + ":";
}

std::string RedisNamespace::ScenePrefix() const
{
    if (sceneId == 0)
        return ZonePrefix();
    return ZonePrefix() + "scene:" + std::to_string(sceneId) + ":";
}

std::string RedisNamespace::ZoneKey(const std::string& name) const
{
    return ZonePrefix() + name;
}

std::string RedisNamespace::SceneKey(const std::string& name) const
{
    return ScenePrefix() + name;
}

std::string RedisNamespace::PlayerKey(uint64_t playerId, const std::string& name) const
{
    // 玩家 key 不带 scene：玩家会跨场景移动，key 必须跟着人走
    return ZonePrefix() + "player:" + std::to_string(playerId) + ":" + name;
}

bool RedisNamespace::Owns(const std::string& key) const
{
    if (!Valid())
        return false;

    const std::string prefix = ZonePrefix();
    return key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

RedisKeyShape RedisCommandKeyShape(const std::string& command)
{
    const std::string cmd = ToUpper(command);
    if (IsNoKeyCommand(cmd))
        return RedisKeyShape::kNoKey;
    if (IsAllArgsKeysCommand(cmd))
        return RedisKeyShape::kAllArgsAreKeys;
    if (IsMsetCommand(cmd))
        return RedisKeyShape::kMsetPairs;
    if (IsEvalCommand(cmd))
        return RedisKeyShape::kEvalKeys;

    // 未列出的命令：按"key 在第一个参数"处理（Redis 绝大多数命令是这个形态）。
    // 想收紧就把命令补进上面的表；放宽则等于放弃 key 校验。
    return RedisKeyShape::kSingleKey;
}

size_t RedisShardOf(const std::string& key, size_t shardCount)
{
    if (shardCount <= 1)
        return 0;
    return static_cast<size_t>(HashKey(key) % shardCount);
}

bool ValidateRedisCommand(const std::vector<std::string>& args,
                          const RedisNamespace& ns,
                          size_t shardCount,
                          size_t& shardOut,
                          std::string& err)
{
    shardOut = 0;
    err.clear();   // 成功时 err 必须是空的：调用方会用 err.empty() 判断结果

    if (args.empty() || args[0].empty())
    {
        err = "empty redis command";
        return false;
    }
    if (shardCount == 0)
        shardCount = 1;

    const RedisKeyShape shape = RedisCommandKeyShape(args[0]);
    std::vector<const std::string*> keys;

    switch (shape)
    {
        case RedisKeyShape::kNoKey:
            break;

        case RedisKeyShape::kSingleKey:
            if (args.size() < 2)
            {
                err = "redis command '" + args[0] + "' requires a key";
                return false;
            }
            keys.push_back(&args[1]);
            break;

        case RedisKeyShape::kAllArgsAreKeys:
            if (args.size() < 2)
            {
                err = "redis command '" + args[0] + "' requires at least one key";
                return false;
            }
            for (size_t i = 1; i < args.size(); ++i)
                keys.push_back(&args[i]);
            break;

        case RedisKeyShape::kMsetPairs:
            if (args.size() < 3 || (args.size() - 1) % 2 != 0)
            {
                err = "redis command '" + args[0] + "' requires key/value pairs";
                return false;
            }
            for (size_t i = 1; i < args.size(); i += 2)
                keys.push_back(&args[i]);
            break;

        case RedisKeyShape::kEvalKeys:
        {
            if (args.size() < 3)
            {
                err = "redis command '" + args[0] + "' requires numkeys";
                return false;
            }
            const long numKeys = std::strtol(args[2].c_str(), nullptr, 10);
            if (numKeys < 0 || static_cast<size_t>(numKeys) > args.size() - 3)
            {
                err = "redis command '" + args[0] + "' has invalid numkeys";
                return false;
            }
            for (size_t i = 3; i < 3 + static_cast<size_t>(numKeys); ++i)
                keys.push_back(&args[i]);
            break;
        }

        default:
            err = "unsupported redis command shape";
            return false;
    }

    if (keys.empty())
    {
        // 与 key 无关的命令：分片按"路由名"（频道 / 命令名）选，
        // 保证同一频道落到同一实例（publish / subscribe 必须同实例）。
        const std::string routeKey = args.size() > 1 ? args[1] : args[0];
        shardOut = RedisShardOf(routeKey, shardCount);
        return true;
    }

    if (!ns.Valid())
    {
        err = "redis namespace not configured (zoneId == 0)";
        return false;
    }

    size_t shard = 0;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const std::string& key = *keys[i];
        if (key.empty())
        {
            err = "redis key must not be empty";
            return false;
        }

        // ★ 核心防线：key 必须落在本区服命名空间内（堵死脚本裸写 key）
        if (!ns.Owns(key))
        {
            err = "redis key outside namespace: '" + key +
                  "' (use db.key.zone/scene/player)";
            return false;
        }

        const size_t s = RedisShardOf(key, shardCount);
        if (i == 0)
        {
            shard = s;
        }
        else if (s != shard)
        {
            // 多 key 命令必须同分片：一次多键操作无法跨实例完成
            err = "redis multi-key command crosses shards: '" + *keys[0] + "' vs '" + key + "'";
            return false;
        }
    }

    shardOut = shard;
    return true;
}
