#include "db/redisKey.h"
#include "test_util.h"

#include <set>
#include <string>
#include <vector>

namespace
{
RedisNamespace Ns(uint32_t zone = 1, uint32_t scene = 2)
{
    RedisNamespace ns;
    ns.zoneId = zone;
    ns.sceneId = scene;
    return ns;
}

std::vector<std::string> Cmd(std::initializer_list<std::string> parts)
{
    return std::vector<std::string>(parts);
}
}  // namespace

TEST(RedisNamespaceKeyLayout)
{
    const RedisNamespace ns = Ns(1, 2);

    CHECK(ns.Valid());
    CHECK(ns.ZonePrefix() == "zone:1:");
    CHECK(ns.ScenePrefix() == "zone:1:scene:2:");
    CHECK(ns.ZoneKey("rank") == "zone:1:rank");
    CHECK(ns.SceneKey("monster") == "zone:1:scene:2:monster");
    // 玩家 key 不带 scene：玩家会跨场景移动，key 必须跟着人走
    CHECK(ns.PlayerKey(1001, "bag") == "zone:1:player:1001:bag");

    // sceneId == 0：退化为区服级前缀（区服级服务用）
    const RedisNamespace zoneOnly = Ns(1, 0);
    CHECK(zoneOnly.ScenePrefix() == "zone:1:");
    CHECK(zoneOnly.SceneKey("k") == "zone:1:k");

    // Owns：只要求落在本区服的命名空间内（场景服写区服级排行榜是合法用法）
    CHECK(ns.Owns("zone:1:scene:2:bag"));
    CHECK(ns.Owns("zone:1:rank"));
    CHECK(!ns.Owns("zone:2:rank"));       // 别的区服
    CHECK(!ns.Owns("bag"));               // 裸 key
    CHECK(!ns.Owns("zone:1"));            // 前缀不完整

    // 未配置命名空间（zoneId == 0）时不认任何 key
    const RedisNamespace invalid;
    CHECK(!invalid.Valid());
    CHECK(!invalid.Owns("zone:0:k"));
}

// 命令的 key 形态识别（注意：用例名不能与全局函数 RedisCommandKeyShape 同名，
// 否则 TEST 宏里声明的 static void Xxx() 会和它构成重载集合，编译期就报"unresolved overload"）
TEST(RedisKeyShapeDetection)
{
    CHECK(RedisCommandKeyShape("HSET") == RedisKeyShape::kSingleKey);
    CHECK(RedisCommandKeyShape("hset") == RedisKeyShape::kSingleKey);   // 大小写不敏感
    CHECK(RedisCommandKeyShape("set") == RedisKeyShape::kSingleKey);
    // 未列出的命令按"key 在第一个参数"处理
    CHECK(RedisCommandKeyShape("LPOS") == RedisKeyShape::kSingleKey);

    CHECK(RedisCommandKeyShape("PING") == RedisKeyShape::kNoKey);
    CHECK(RedisCommandKeyShape("PUBLISH") == RedisKeyShape::kNoKey);

    CHECK(RedisCommandKeyShape("DEL") == RedisKeyShape::kAllArgsAreKeys);
    CHECK(RedisCommandKeyShape("MGET") == RedisKeyShape::kAllArgsAreKeys);

    CHECK(RedisCommandKeyShape("MSET") == RedisKeyShape::kMsetPairs);
    CHECK(RedisCommandKeyShape("EVAL") == RedisKeyShape::kEvalKeys);
}

// ★ 核心：脚本裸写 key 必须被拒绝（评审 D3）
TEST(RedisRejectsRawKey)
{
    const RedisNamespace ns = Ns(1, 2);
    size_t shard = 0;
    std::string err;

    CHECK(!ValidateRedisCommand(Cmd({"SET", "bag:1001", "1"}), ns, 1, shard, err));
    CHECK(err.find("outside namespace") != std::string::npos);

    // 别的区服的 key 也不行
    CHECK(!ValidateRedisCommand(Cmd({"SET", "zone:2:bag", "1"}), ns, 1, shard, err));

    // 空 key / 空命令 / 缺 key
    CHECK(!ValidateRedisCommand(Cmd({"SET", "", "1"}), ns, 1, shard, err));
    CHECK(!ValidateRedisCommand({}, ns, 1, shard, err));
    CHECK(!ValidateRedisCommand(Cmd({"SET"}), ns, 1, shard, err));
}

TEST(RedisAcceptsNamespacedKey)
{
    const RedisNamespace ns = Ns(1, 2);
    size_t shard = 0;
    std::string err;

    CHECK(ValidateRedisCommand(Cmd({"SET", ns.SceneKey("bag"), "1"}), ns, 1, shard, err));
    CHECK(err.empty());
    CHECK_EQ(shard, (size_t)0);

    // 区服级 key（跨场景共享，例如排行榜）同样合法
    CHECK(ValidateRedisCommand(Cmd({"ZADD", ns.ZoneKey("rank"), "1", "p1"}), ns, 1, shard, err));
    CHECK(err.empty());

    // 与 key 无关的命令不需要命名空间
    const RedisNamespace none;
    CHECK(ValidateRedisCommand(Cmd({"PING"}), none, 1, shard, err));

    // 多 key 命令：全部命中命名空间则通过
    CHECK(ValidateRedisCommand(Cmd({"MGET", ns.SceneKey("a"), ns.SceneKey("b")}), ns, 1, shard, err));
    CHECK(err.empty());

    // MSET 是 key/value 成对
    CHECK(ValidateRedisCommand(
        Cmd({"MSET", ns.SceneKey("a"), "1", ns.SceneKey("b"), "2"}), ns, 1, shard, err));
    CHECK(err.empty());
    CHECK(!ValidateRedisCommand(Cmd({"MSET", ns.SceneKey("a"), "1", ns.SceneKey("b")}), ns, 1,
                               shard, err));   // 缺 value

    // EVAL：numkeys 决定 key 的位置
    CHECK(ValidateRedisCommand(Cmd({"EVAL", "return 1", "1", ns.SceneKey("a")}), ns, 1, shard, err));
    CHECK(err.empty());
    CHECK(!ValidateRedisCommand(Cmd({"EVAL", "return 1", "2", ns.SceneKey("a")}), ns, 1, shard, err));

    // 未配置命名空间时，带 key 的命令必须失败
    CHECK(!ValidateRedisCommand(Cmd({"SET", "zone:1:k", "1"}), none, 1, shard, err));
    CHECK(err.find("namespace not configured") != std::string::npos);
}

// ★ 分片不变量：同一个 key 永远落在同一个分片（与写入顺序、进程重启无关）
TEST(RedisShardOfKeyIsStable)
{
    constexpr size_t kShards = 4;
    const std::string key = "zone:1:scene:2:bag";

    const size_t first = RedisShardOf(key, kShards);
    for (int i = 0; i < 100; ++i)
        CHECK_EQ(RedisShardOf(key, kShards), first);

    // 单实例 / 0 分片时恒为 0
    CHECK_EQ(RedisShardOf(key, 0), (size_t)0);
    CHECK_EQ(RedisShardOf(key, 1), (size_t)0);

    // 大量 key 应能铺开到多个分片（而不是全部堆在 0 号）
    const RedisNamespace ns = Ns(1, 2);
    std::set<size_t> used;
    for (uint64_t pid = 1; pid <= 200; ++pid)
        used.insert(RedisShardOf(ns.PlayerKey(pid, "bag"), kShards));
    CHECK(used.size() >= kShards);
}

// 多 key 命令跨分片必须被拒绝（Redis 无法跨实例完成一次多键操作）
TEST(RedisMultiKeyCrossShardRejected)
{
    const RedisNamespace ns = Ns(1, 2);
    constexpr size_t kShards = 4;

    // 找出两个落在不同分片上的 key
    std::string keyA;
    std::string keyB;
    for (uint64_t pid = 1; pid <= 200 && keyB.empty(); ++pid)
    {
        const std::string k = ns.PlayerKey(pid, "bag");
        if (keyA.empty())
        {
            keyA = k;
            continue;
        }
        if (RedisShardOf(k, kShards) != RedisShardOf(keyA, kShards))
            keyB = k;
    }
    CHECK(!keyB.empty());

    size_t shard = 0;
    std::string err;
    CHECK(!ValidateRedisCommand(Cmd({"DEL", keyA.c_str(), keyB.c_str()}), ns, kShards, shard, err));
    CHECK(err.find("crosses shards") != std::string::npos);

    // 同分片（同一个 key）则允许，且分片就是它的分片
    CHECK(ValidateRedisCommand(Cmd({"DEL", keyA.c_str(), keyA.c_str()}), ns, kShards, shard, err));
    CHECK_EQ(shard, RedisShardOf(keyA, kShards));
}

// 带 key 的命令：目标分片必须等于"按 key 算出的分片"
TEST(RedisCommandShardIsKeyShard)
{
    const RedisNamespace ns = Ns(3, 5);
    constexpr size_t kShards = 8;

    const std::string key = ns.PlayerKey(987654, "session");
    size_t shard = 0;
    std::string err;
    CHECK(ValidateRedisCommand(Cmd({"HSET", key.c_str(), "f", "v"}), ns, kShards, shard, err));
    CHECK_EQ(shard, RedisShardOf(key, kShards));

    // 与 key 无关的命令（PING/PUBLISH）按"路由名"分片，且有界
    CHECK(ValidateRedisCommand(Cmd({"PING"}), ns, kShards, shard, err));
    CHECK(shard < kShards);
    CHECK(ValidateRedisCommand(Cmd({"PUBLISH", ns.ZoneKey("chan"), "hi"}), ns, kShards, shard, err));
    CHECK(shard < kShards);
}
