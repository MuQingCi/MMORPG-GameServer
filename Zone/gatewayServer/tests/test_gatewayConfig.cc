#include "gatewayConfig.h"
#include "test_util.h"
#include "base/yaml.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// ---------------------------------------------------------------------------
// 网关配置：**两个监听端口**（公网/内网）、后端白名单、迁移守卫
//
// 这些用例把"区分两类数据"的配置前提固化下来：
//   - 公网端口配错 -> 客户端连不上；内网端口配错 -> 场景服反向连接失败；
//   - 两端口相同 / 白名单缺失 / 默认目标不在白名单里 -> 必须启动即失败，
//     而不是带着错配置跑起来（那会表现成"偶发收不到包"）。
// ---------------------------------------------------------------------------
namespace
{
const char* kTmpPath = "/tmp/clearmoon_gateway_cfg_test.yaml";

bool WriteFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path);
    if (!out)
        return false;
    out << text;
    out.close();   // 必须先落盘：Load 是重新打开文件读的
    return true;
}

std::string RepoConfig(const char* name)
{
    return std::string(GATEWAY_SOURCE_DIR) + "/config/" + name;
}

// 最小可用配置（端口可注入）
std::string Cfg(const std::string& publicPort, const std::string& privatePort,
                const std::string& extra = "")
{
    return "gateway:\n"
           "    zoneId: 1\n"
           "    gatewayId: 2\n"
           "    publicConfig:\n"
           "        listenAddr: 127.0.0.1\n"
           "        listenPort: " + publicPort + "\n"
           "    privateConfig:\n"
           "        listenAddr: 127.0.0.1\n"
           "        listenPort: " + privatePort + "\n"
           "        allowedServices:\n"
           "            - 8\n"
           "    eventThreadNum: 2\n" + extra;
}

bool Load(const std::string& path, GatewayConfig& cfg, std::string& err)
{
    return GatewayConfig::Load(path, cfg, err);
}
}  // namespace

// 读仓库里的真实配置（与部署用的是同一份）
TEST(GatewayConfigLoadFromRealFile)
{
    GatewayConfig cfg;
    std::string err;
    CHECK(Load(RepoConfig("gatewayConfig.yaml"), cfg, err));
    if (!err.empty())
        std::fprintf(stderr, "gateway config err: %s\n", err.c_str());

    CHECK_EQ(cfg.zoneId, (uint32_t)1);
    CHECK_EQ(cfg.gatewayId, (uint32_t)1);
    CHECK(cfg.publicListenAddr == "127.0.0.1");
    CHECK_EQ(cfg.publicListenPort, (uint16_t)10000);
    CHECK(cfg.privateListenAddr == "127.0.0.1");
    // ★ 内网端口必须与 config/zoneConfig.yaml 的 gateways 一致（否则场景服连不上）
    CHECK_EQ(cfg.privateListenPort, (uint16_t)13145);
    CHECK_EQ(cfg.allowedServices.size(), (size_t)1);
    CHECK_EQ(cfg.allowedServices[0], (uint8_t)8);
    CHECK_EQ(cfg.defaultServiceId, (uint8_t)8);
    CHECK_EQ(cfg.clientToken, (uint64_t)998877);
    CHECK_EQ(cfg.eventThreadNum, (uint32_t)4);
    CHECK(cfg.logLevel == "info");
}

// 直接读取两份部署用配置，防止后端目标误指向客户端公网端口。
TEST(GatewayPrivateEndpointMatchesZoneConfig)
{
    GatewayConfig gateway;
    std::string err;
    const bool loaded = GatewayConfig::Load(RepoConfig("gatewayConfig.yaml"), gateway, err);
    CHECK(loaded);
    if (!loaded) return;

    std::ifstream in(std::string(GATEWAY_SOURCE_DIR) + "/../config/zoneConfig.yaml");
    CHECK(in.good());
    if (!in.good()) return;
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    YamlLite yaml;
    const bool parsed = yaml.Parse(content, err);
    CHECK(parsed);
    if (!parsed) return;
    const auto endpoints = yaml.GetList("ZoneServer.gateways");
    CHECK(!endpoints.empty());
    for (const auto& endpoint : endpoints)
    {
        const auto separator = endpoint.rfind(':');
        CHECK(separator != std::string::npos);
        if (separator == std::string::npos) continue;
        CHECK_EQ(endpoint.substr(0, separator), gateway.privateListenAddr);
        CHECK_EQ(endpoint.substr(separator + 1), std::to_string(gateway.privateListenPort));
    }
}

// 改端口要能生效
TEST(GatewayConfigPortOverride)
{
    CHECK(WriteFile(kTmpPath, Cfg("20001", "20002")));

    GatewayConfig cfg;
    std::string err;
    CHECK(Load(kTmpPath, cfg, err));
    CHECK_EQ(cfg.publicListenPort, (uint16_t)20001);
    CHECK_EQ(cfg.privateListenPort, (uint16_t)20002);
    CHECK_EQ(cfg.gatewayId, (uint32_t)2);
    std::remove(kTmpPath);
}

// 键名拼错必须报错（而不是静默用默认值）
TEST(GatewayConfigRejectsTypoKey)
{
    CHECK(WriteFile(kTmpPath,
                    "gateway:\n"
                    "    zoneId: 1\n"
                    "    gatewayId: 2\n"
                    "    publicConfig:\n"
                    "        listenAddr: 127.0.0.1\n"
                    "        listenProt: 20001\n"
                    "    privateConfig:\n"
                    "        listenAddr: 127.0.0.1\n"
                    "        listenPort: 20002\n"
                    "        allowedServices:\n"
                    "            - 8\n"
                    "    eventThreadNum: 2\n"));

    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("listenProt") != std::string::npos);
    std::remove(kTmpPath);
}

// 迁移守卫：旧的平铺键（gateway.listenAddr/listenPort）必须被拒绝，
// 否则"改了配置却没生效"会以最难查的方式复现（老键被读、新键被忽略）。
TEST(GatewayConfigRejectsLegacyFlatKeys)
{
    CHECK(WriteFile(kTmpPath,
                    "gateway:\n"
                    "    zoneId: 1\n"
                    "    gatewayId: 1\n"
                    "    listenAddr: 127.0.0.1\n"
                    "    listenPort: 10000\n"
                    "    publicConfig:\n"
                    "        listenPort: 20001\n"
                    "    privateConfig:\n"
                    "        listenPort: 20002\n"
                    "        allowedServices:\n"
                    "            - 8\n"
                    "    eventThreadNum: 2\n"));

    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("gateway.listenPort") != std::string::npos);
    std::remove(kTmpPath);
}

// 旧键名（zoneID 大写 ID）必须被拒绝：键名已统一为小驼峰
TEST(GatewayConfigRejectsLegacyKeyName)
{
    CHECK(WriteFile(kTmpPath,
                    "gateway:\n"
                    "    zoneID: 1\n"
                    "    gatewayId: 1\n"
                    "    publicConfig:\n"
                    "        listenPort: 20001\n"
                    "    privateConfig:\n"
                    "        listenPort: 20002\n"
                    "        allowedServices:\n"
                    "            - 8\n"
                    "    eventThreadNum: 2\n"));

    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("zoneID") != std::string::npos);
    std::remove(kTmpPath);
}

// 端口越界必须被拒绝。
// 注意：`(uint16_t)y.GetUInt(...)` 会把 70000 静默截断成 4464（70000 % 65536），
// 校验发生在截断之后等于没校验 —— 这是实际抓到过的缺陷，用示例固化：
TEST(GatewayConfigRejectsOutOfRangePort)
{
    CHECK(WriteFile(kTmpPath, Cfg("70000", "20002")));
    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("65535") != std::string::npos);
    std::remove(kTmpPath);

    // 0 也是非法端口
    CHECK(WriteFile(kTmpPath, Cfg("0", "20002")));
    GatewayConfig cfg2;
    std::string err2;
    CHECK(!Load(kTmpPath, cfg2, err2));
    std::remove(kTmpPath);
}

// 合法上界必须被接受（避免"一刀切拒绝"式的过度修复）
TEST(GatewayConfigAcceptsBoundaryPort)
{
    CHECK(WriteFile(kTmpPath, Cfg("65535", "20002")));

    GatewayConfig cfg;
    std::string err;
    CHECK(Load(kTmpPath, cfg, err));
    CHECK_EQ(cfg.publicListenPort, (uint16_t)65535);
    std::remove(kTmpPath);
}

// 线程数为 0 必须被拒绝（acceptor 分配不到 loop）
TEST(GatewayConfigRejectsZeroThreadNum)
{
    CHECK(WriteFile(kTmpPath,
                    "gateway:\n"
                    "    zoneId: 1\n"
                    "    gatewayId: 1\n"
                    "    publicConfig:\n"
                    "        listenPort: 20001\n"
                    "    privateConfig:\n"
                    "        listenPort: 20002\n"
                    "        allowedServices:\n"
                    "            - 8\n"
                    "    eventThreadNum: 0\n"));

    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("eventThreadNum") != std::string::npos);
    std::remove(kTmpPath);
}

// 后端白名单必须显式配置（安全边界不拿默认值兜底）
TEST(GatewayConfigRequiresAllowedServices)
{
    CHECK(WriteFile(kTmpPath,
                    "gateway:\n"
                    "    zoneId: 1\n"
                    "    gatewayId: 1\n"
                    "    publicConfig:\n"
                    "        listenPort: 20001\n"
                    "    privateConfig:\n"
                    "        listenPort: 20002\n"
                    "    eventThreadNum: 2\n"));

    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("allowedServices") != std::string::npos);
    std::remove(kTmpPath);

    // 行内 [8] 会被本解析器当标量：必须报错并给出正确写法，而不是静默"白名单为空"
    CHECK(WriteFile(kTmpPath,
                    "gateway:\n"
                    "    zoneId: 1\n"
                    "    gatewayId: 1\n"
                    "    publicConfig:\n"
                    "        listenPort: 20001\n"
                    "    privateConfig:\n"
                    "        listenPort: 20002\n"
                    "        allowedServices: [8]\n"
                    "    eventThreadNum: 2\n"));

    GatewayConfig cfg2;
    std::string err2;
    CHECK(!Load(kTmpPath, cfg2, err2));
    CHECK(err2.find("must be a YAML list") != std::string::npos);
    std::remove(kTmpPath);
}

// 两个监听端口相同必须被拒绝：那等于让两类连接落到同一个端口上（协议必然串流）
TEST(GatewayConfigRejectsSamePorts)
{
    CHECK(WriteFile(kTmpPath, Cfg("20001", "20001")));

    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("must differ") != std::string::npos);
    std::remove(kTmpPath);
}

// 默认后端服务必须落在白名单里，否则客户端每个请求都会 "service unavailable"
TEST(GatewayConfigRejectsDefaultNotAllowed)
{
    CHECK(WriteFile(kTmpPath, Cfg("20001", "20002", "    defaultServiceId: 3\n")));

    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("defaultServiceId") != std::string::npos);
    std::remove(kTmpPath);
}

// 日志级别拼错必须报错：静默回落默认级别会让"我明明开了 debug"变成空话
TEST(GatewayConfigRejectsUnknownLogLevel)
{
    CHECK(WriteFile(kTmpPath, Cfg("20001", "20002", "    logLevel: warning\n")));

    GatewayConfig cfg;
    std::string err;
    CHECK(!Load(kTmpPath, cfg, err));
    CHECK(err.find("logLevel") != std::string::npos);
    std::remove(kTmpPath);

    // 合法值必须被接受
    CHECK(WriteFile(kTmpPath, Cfg("20001", "20002", "    logLevel: debug\n")));
    GatewayConfig cfg2;
    std::string err2;
    CHECK(Load(kTmpPath, cfg2, err2));
    CHECK(cfg2.logLevel == "debug");
    std::remove(kTmpPath);
}

// 文件不存在必须明确报错
TEST(GatewayConfigMissingFile)
{
    GatewayConfig cfg;
    std::string err;
    CHECK(!Load("/tmp/clearmoon_no_such_gateway_cfg.yaml", cfg, err));
    CHECK(!err.empty());
}
