#include "base/yaml.h"
#include "config/sceneConfig.h"
#include "config/zoneConfig.h"
#include "test_util.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace
{
const char* kTmpPath = "/tmp/clearmoon_test_cfg.yaml";

bool WriteFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path);
    if (!out)
        return false;
    out << text;
    out.close();   // 必须先落盘：Load 是重新打开文件读的
    return true;
}

// 仓库里的真实配置文件路径（由 CMake 注入 SCENE_SOURCE_DIR）
std::string RepoConfig(const char* name)
{
    if (std::string(name) == "zoneConfig.yaml")
        return std::string(SCENE_SOURCE_DIR) + "/../config/" + name;
    return std::string(SCENE_SOURCE_DIR) + "/config/" + name;
}
}  // namespace

TEST(YamlLiteBasics)
{
    const std::string text =
        "# 顶层注释\n"
        "Root:\n"
        "    name: scene-1      # 行尾注释\n"
        "    num: 42\n"
        "    flag: true\n"
        "    ratio: 1.5\n"
        "    quoted: \"a # b\"\n"
        "    list:\n"
        "        - 127.0.0.1:10000\n"
        "        - 127.0.0.1:10001\n"
        "    nested:\n"
        "        deep: ok\n";

    YamlLite y;
    std::string err;
    CHECK(y.Parse(text, err));
    CHECK(err.empty());

    CHECK(y.GetStr("Root.name") == "scene-1");
    CHECK_EQ(y.GetInt("Root.num"), (int64_t)42);
    CHECK(y.GetBool("Root.flag"));
    CHECK_EQ(y.GetDouble("Root.ratio"), 1.5);
    CHECK(y.GetStr("Root.quoted") == "a # b");
    CHECK(y.GetStr("Root.nested.deep") == "ok");
    CHECK_EQ(y.GetList("Root.list").size(), (size_t)2);
    CHECK(y.GetList("Root.list")[1] == "127.0.0.1:10001");

    // 不存在的键返回默认值
    CHECK_EQ(y.GetInt("Root.missing", -7), (int64_t)-7);

    // 全部键都被读过 -> 没有"未被消费"的键
    CHECK(y.UnusedKeys().empty());
}

// 键名拼错必须能被列出来，否则会静默回落默认值（评审 B2）
TEST(YamlLiteUnusedKeys)
{
    YamlLite y;
    std::string err;
    CHECK(y.Parse("Root:\n    sceneId: 1\n    sceneID: 2\n", err));

    CHECK_EQ(y.GetUInt("Root.sceneId", 0), (uint64_t)1);

    const std::vector<std::string> unused = y.UnusedKeys();
    CHECK_EQ(unused.size(), (size_t)1);
    if (!unused.empty())
        CHECK(unused[0] == "Root.sceneID");

    // 前缀查询能发现"某一段是否还在这个文件里"（容器键本身没有值，Has() 看不到）
    CHECK(y.HasPrefix("Root."));
    CHECK(!y.HasPrefix("Other."));
}

TEST(YamlLiteRejectsTab)
{
    YamlLite y;
    std::string err;
    CHECK(!y.Parse("Root:\n\tname: bad\n", err));
    CHECK(!err.empty());
}

// ---------------------------------------------------------------------------
// 区服配置：直接读仓库里的真实文件
//   这是 A2/A3/D2 类问题的守门测试：yaml 用了 Tab、键名写错、段没配全、
//   dbName 为空，都会在这里暴露，而不是等部署时才发现连不上库。
// ---------------------------------------------------------------------------
TEST(ZoneConfigLoadFromRealFile)
{
    ZoneConfig z;
    std::string err;
    CHECK(ZoneConfig::Load(RepoConfig("zoneConfig.yaml"), z, err));
    if (!err.empty())
        std::fprintf(stderr, "zone config err: %s\n", err.c_str());

    CHECK_EQ(z.zoneId, (uint32_t)1);
    CHECK(z.dbEnable);
    CHECK(!z.db.mysql.dbName.empty());     // ★ 抓"dbName 为空还能起来"
    CHECK(!z.db.mysql.host.empty());
    CHECK(!z.logName.empty());             // ★ 抓"多服务写同一日志文件"
    CHECK(!z.gateways.empty());            // 网关列表已从 sceneConfig 迁到这里
    CHECK_EQ(z.gateways.size(), (size_t)1);
    if (!z.gateways.empty())
        CHECK_EQ(z.gateways[0].port, (uint16_t)10000);

    // Redis 命名空间的 zoneId 必须由 zoneId 派生（不单独配，避免两处不一致）
    CHECK_EQ(z.db.redis.zoneId, z.zoneId);
    CHECK_EQ(z.db.redis.sceneId, (uint32_t)0);     // 区服级服务保持 0
    CHECK_EQ(z.db.redis.ShardCount(), (size_t)1);  // 未配 shards -> 单实例

    std::string host;
    uint16_t port = 0;
    CHECK(z.db.redis.EndpointOf(0, host, port));
    CHECK_EQ(port, z.db.redis.port);
}

// 缺 dbName 时必须拒绝（否则会静默连空库 —— 评审 A3 的同类问题）
TEST(ZoneConfigRejectsEmptyDbName)
{
    const std::string text =
        "ZoneServer:\n"
        "    zoneId: 1\n"
        "    db:\n"
        "        enable: true\n"
        "        mysql:\n"
        "            host: 127.0.0.1\n"
        "            user: root\n"
        "    log:\n"
        "        name: scene\n";
    CHECK(WriteFile(kTmpPath, text));

    ZoneConfig z;
    std::string err;
    CHECK(!ZoneConfig::Load(kTmpPath, z, err));
    CHECK(err.find("dbName") != std::string::npos);
    std::remove(kTmpPath);
}

// 日志名必须显式配置（多服务共用 zoneConfig 时不能都叫同一个名字）
TEST(ZoneConfigRejectsEmptyLogName)
{
    const std::string text =
        "ZoneServer:\n"
        "    zoneId: 1\n"
        "    db:\n"
        "        enable: false\n"
        "    log:\n"
        "        dir: ./logs\n";
    CHECK(WriteFile(kTmpPath, text));

    ZoneConfig z;
    std::string err;
    CHECK(!ZoneConfig::Load(kTmpPath, z, err));
    CHECK(err.find("log.name") != std::string::npos);
    std::remove(kTmpPath);
}

// 未知键 / 拼错的键必须报错（评审 B2 的防线）
TEST(ZoneConfigRejectsUnknownKey)
{
    const std::string text =
        "ZoneServer:\n"
        "    zoneId: 1\n"
        "    dbEnable: true\n"      // 应为 db.enable
        "    log:\n"
        "        name: scene\n";
    CHECK(WriteFile(kTmpPath, text));

    ZoneConfig z;
    std::string err;
    CHECK(!ZoneConfig::Load(kTmpPath, z, err));
    CHECK(err.find("dbEnable") != std::string::npos);
    std::remove(kTmpPath);
}

// Redis 多实例分片：shards 必须能解析出端点，越界分片必须失败
TEST(ZoneConfigRedisShards)
{
    const std::string text =
        "ZoneServer:\n"
        "    zoneId: 7\n"
        "    db:\n"
        "        enable: true\n"
        "        mysql:\n"
        "            dbName: game\n"
        "            user: root\n"
        "        redis:\n"
        "            password: secret\n"
        "            shards:\n"
        "                - 10.0.0.1:6379\n"
        "                - 10.0.0.2:6380\n"
        "    log:\n"
        "        name: scene\n";
    CHECK(WriteFile(kTmpPath, text));

    ZoneConfig z;
    std::string err;
    CHECK(ZoneConfig::Load(kTmpPath, z, err));
    CHECK_EQ(z.db.redis.ShardCount(), (size_t)2);
    CHECK_EQ(z.db.redis.zoneId, (uint32_t)7);   // 从 zoneId 派生

    std::string host;
    uint16_t port = 0;
    CHECK(z.db.redis.EndpointOf(1, host, port));
    CHECK(host == "10.0.0.2");
    CHECK_EQ(port, (uint16_t)6380);

    // 越界分片必须失败（调用方据此报错，而不是静默落到 0 号）
    CHECK(!z.db.redis.EndpointOf(2, host, port));
    std::remove(kTmpPath);
}

// 非法分片端点必须被拦下
TEST(ZoneConfigRejectsBadShard)
{
    const std::string text =
        "ZoneServer:\n"
        "    zoneId: 1\n"
        "    db:\n"
        "        enable: true\n"
        "        mysql:\n"
        "            dbName: game\n"
        "            user: root\n"
        "        redis:\n"
        "            shards:\n"
        "                - 10.0.0.1\n"      // 缺端口
        "    log:\n"
        "        name: scene\n";
    CHECK(WriteFile(kTmpPath, text));

    ZoneConfig z;
    std::string err;
    CHECK(!ZoneConfig::Load(kTmpPath, z, err));
    CHECK(err.find("shards") != std::string::npos);
    std::remove(kTmpPath);
}

// ---------------------------------------------------------------------------
// 场景服配置：读真实文件 + 与 zone 合并后的派生字段
// ---------------------------------------------------------------------------
TEST(SceneConfigLoadAndAttachZone)
{
    ZoneConfig zone;
    std::string err;
    CHECK(ZoneConfig::Load(RepoConfig("zoneConfig.yaml"), zone, err));

    SceneConfig cfg;
    CHECK(SceneConfig::Load(RepoConfig("sceneConfig.yaml"), cfg, err));
    if (!err.empty())
        std::fprintf(stderr, "scene config err: %s\n", err.c_str());

    CHECK_EQ(cfg.sceneId, (uint32_t)1);
    CHECK_EQ(cfg.logicThreadNum, (uint32_t)4);
    CHECK(!cfg.luaDir.empty());
    // 尚未 AttachZone：区服共享字段还是默认值（网关列表为空）
    CHECK(cfg.zone.gateways.empty());
    CHECK_EQ(cfg.zone.zoneId, (uint32_t)0);   // 默认 0 = 未配置（Validate 会拒绝）

    CHECK(cfg.AttachZone(zone, err));
    if (!err.empty())
        std::fprintf(stderr, "attach err: %s\n", err.c_str());

    CHECK_EQ(cfg.zone.gateways.size(), (size_t)1);   // 已并入区服配置

    // 派生字段：一次配置、多处一致
    CHECK_EQ(cfg.net.zoneId, zone.zoneId);
    CHECK_EQ(cfg.net.sceneId, cfg.sceneId);
    CHECK_EQ(cfg.net.serviceId, cfg.serviceId);

    // Redis 命名空间：zone 来自 ZoneConfig，scene 来自本场景
    const RedisNamespace ns = cfg.RedisNs();
    CHECK_EQ(ns.zoneId, zone.zoneId);
    CHECK_EQ(ns.sceneId, cfg.sceneId);
    CHECK(ns.Valid());
    CHECK(ns.SceneKey("bag") == "zone:1:scene:1:bag");
    CHECK(ns.ZoneKey("rank") == "zone:1:rank");
    CHECK(ns.PlayerKey(1001, "bag") == "zone:1:player:1001:bag");
}

// 已迁移到 zoneConfig 的键必须被明确拒绝 —— 防止"同一项写两处"
TEST(SceneConfigRejectsMigratedKeys)
{
    const char* kMigrated[] = {
        "SceneServer:\n    sceneId: 1\n    zoneId: 2\n",
        "SceneServer:\n    sceneId: 1\n    db:\n        enable: true\n",
        "SceneServer:\n    sceneId: 1\n    gateways:\n        - 127.0.0.1:10000\n",
        "SceneServer:\n    sceneId: 1\n    log:\n        dir: ./logs\n",
    };

    for (const char* text : kMigrated)
    {
        CHECK(WriteFile(kTmpPath, text));
        SceneConfig cfg;
        std::string err;
        const bool ok = SceneConfig::Load(kTmpPath, cfg, err);
        CHECK(!ok);
        CHECK(err.find("zoneConfig.yaml") != std::string::npos);
        std::remove(kTmpPath);
    }
}

// 端口越界必须被拒绝。
// 注意：`(uint16_t)y.GetUInt(...)` 会把 70000 静默截断成 4464（70000 % 65536），
// 且校验发生在截断之后 —— 配置"加载成功"但连到/监听在完全不同的端口上。
TEST(ZoneConfigRejectsOutOfRangePort)
{
    // mysql.port 越界
    {
        const std::string text =
            "ZoneServer:\n"
            "    zoneId: 1\n"
            "    db:\n"
            "        enable: true\n"
            "        mysql:\n"
            "            dbName: game\n"
            "            user: root\n"
            "            port: 70000\n"      // 越界
            "    log:\n"
            "        name: scene\n";
        CHECK(WriteFile(kTmpPath, text));

        ZoneConfig z;
        std::string err;
        CHECK(!ZoneConfig::Load(kTmpPath, z, err));
        CHECK(err.find("65535") != std::string::npos);
        std::remove(kTmpPath);
    }

    // redis.port = 0（非法）
    {
        const std::string text =
            "ZoneServer:\n"
            "    zoneId: 1\n"
            "    db:\n"
            "        enable: true\n"
            "        mysql:\n"
            "            dbName: game\n"
            "            user: root\n"
            "        redis:\n"
            "            port: 0\n"
            "    log:\n"
            "        name: scene\n";
        CHECK(WriteFile(kTmpPath, text));

        ZoneConfig z;
        std::string err;
        CHECK(!ZoneConfig::Load(kTmpPath, z, err));
        CHECK(err.find("65535") != std::string::npos);
        std::remove(kTmpPath);
    }
}

// 端口合法上界必须被接受（避免"一刀切拒绝"式的过度修复）
TEST(ZoneConfigAcceptsBoundaryPort)
{
    const std::string text =
        "ZoneServer:\n"
        "    zoneId: 1\n"
        "    db:\n"
        "        enable: true\n"
        "        mysql:\n"
        "            dbName: game\n"
        "            user: root\n"
        "            port: 65535\n"
        "    log:\n"
        "        name: scene\n";
    CHECK(WriteFile(kTmpPath, text));

    ZoneConfig z;
    std::string err;
    CHECK(ZoneConfig::Load(kTmpPath, z, err));
    CHECK_EQ(z.db.mysql.port, (uint16_t)65535);
    std::remove(kTmpPath);
}

// 场景服监听端口越界必须被拒绝（同一缺陷类的第三处）
TEST(SceneConfigRejectsOutOfRangeListenPort)
{
    const std::string text =
        "SceneServer:\n"
        "    sceneId: 1\n"
        "    net:\n"
        "        listenPort: 70000\n";      // 越界
    CHECK(WriteFile(kTmpPath, text));

    SceneConfig cfg;
    std::string err;
    CHECK(!SceneConfig::Load(kTmpPath, cfg, err));
    CHECK(err.find("65535") != std::string::npos);
    std::remove(kTmpPath);
}

// 场景服监听端口合法上界
TEST(SceneConfigAcceptsBoundaryListenPort)
{
    const std::string text =
        "SceneServer:\n"
        "    sceneId: 1\n"
        "    net:\n"
        "        listenPort: 65535\n";
    CHECK(WriteFile(kTmpPath, text));

    SceneConfig cfg;
    std::string err;
    CHECK(SceneConfig::Load(kTmpPath, cfg, err));
    CHECK_EQ(cfg.net.listenPort, (uint16_t)65535);
    std::remove(kTmpPath);
}

// 场景服文件里的未知键（拼错）必须报错，而不是静默回落默认值
TEST(SceneConfigRejectsUnknownKey)
{
    const std::string text =
        "SceneServer:\n"
        "    sceneID: 3\n"        // 拼错：应为 sceneId
        "    net:\n"
        "        listenEnable: false\n";
    CHECK(WriteFile(kTmpPath, text));

    SceneConfig cfg;
    std::string err;
    CHECK(!SceneConfig::Load(kTmpPath, cfg, err));
    CHECK(err.find("sceneID") != std::string::npos);
    std::remove(kTmpPath);
}

// 既不监听、也不连网关 -> 没有任何入口，必须拒绝
TEST(SceneConfigRejectsNoIngress)
{
    const std::string text =
        "SceneServer:\n"
        "    sceneId: 1\n"
        "    net:\n"
        "        listenEnable: false\n";
    CHECK(WriteFile(kTmpPath, text));

    SceneConfig cfg;
    std::string err;
    CHECK(SceneConfig::Load(kTmpPath, cfg, err));

    ZoneConfig zone;
    zone.zoneId = 1;            // 未配置 zoneId 会被 Validate 拒绝（默认 0）
    zone.logName = "scene";
    zone.dbEnable = false;      // 不校验 db
    zone.gateways.clear();

    CHECK(!cfg.AttachZone(zone, err));
    CHECK(err.find("no way to receive traffic") != std::string::npos);
    std::remove(kTmpPath);
}
