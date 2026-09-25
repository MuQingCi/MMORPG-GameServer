#include "gatewayConfig.h"

#include "base/yaml.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
/**
 * @brief 未知键检查
 *
 * 为什么必须做：YamlLite 在键缺失时返回调用方给的默认值（这是必要的，
 * 否则可选配置无法省略），但这会让**键名拼错**变成"静默使用默认值"。
 * 历史上场景服就出现过"yaml 删了 db 段而代码还在读它"的事故：
 * 配置写了却不生效，而且启动日志还显示一切正常。
 * 这里把所有"没人读过"的标量键列出来直接报错，错误信息里带键名，一眼能定位。
 */
bool CheckUnknownKeys(const YamlLite& y, const std::string& path, std::string& err)
{
    const std::vector<std::string> unused = y.UnusedKeys();
    if (unused.empty())
        return true;

    std::string list;
    for (size_t i = 0; i < unused.size(); ++i)
        list += (i == 0 ? "" : ", ") + unused[i];

    err = path + ": unknown/unused config keys: " + list + "  (typo? or moved to another config file?)";
    return false;
}

/**
 * @brief 读取 1..255 的单字节配置
 *
 * 与端口同理：先按 uint64 取原值再判范围，**不能在窄化之后校验** ——
 * `(uint8_t)GetUInt(key, 300)` 会静默变成 44。
 */
bool ReadU8(const YamlLite& y, const std::string& key, uint8_t def, uint8_t& out, std::string& err)
{
    const uint64_t v = y.GetUInt(key, def);
    if (v == 0 || v > 255)
    {
        err = key + " must be in [1, 255], got " + std::to_string(v);
        return false;
    }
    out = static_cast<uint8_t>(v);
    return true;
}
}  // namespace

bool GatewayConfig::Load(const std::string& path, GatewayConfig& out, std::string& err)
{
    std::ifstream in(path);
    if (!in)
    {
        err = "cannot open config file: " + path;
        return false;
    }

    std::stringstream ss;
    ss << in.rdbuf();

    YamlLite y;
    if (!y.Parse(ss.str(), err))
    {
        err = path + ": " + err;
        return false;
    }

    out.zoneId = (uint32_t)y.GetUInt("gateway.zoneId", out.zoneId);
    out.gatewayId = (uint32_t)y.GetUInt("gateway.gatewayId", out.gatewayId);

    // ---------------- 公网监听（客户端）----------------
    out.publicListenAddr = y.GetStr("gateway.publicConfig.listenAddr", out.publicListenAddr);
    if (!y.GetPort("gateway.publicConfig.listenPort", out.publicListenPort, out.publicListenPort, err))
    {
        err = path + ": " + err;
        return false;
    }

    // ---------------- 内网监听（区服内其它服务器）----------------
    out.privateListenAddr = y.GetStr("gateway.privateConfig.listenAddr", out.privateListenAddr);
    if (!y.GetPort("gateway.privateConfig.listenPort", out.privateListenPort, out.privateListenPort, err))
    {
        err = path + ": " + err;
        return false;
    }

    // 后端服务号白名单：**必须显式配置**（安全边界不拿默认值兜底）
    if (!y.Has("gateway.privateConfig.allowedServices"))
    {
        err = path + ": missing required key: gateway.privateConfig.allowedServices"
                     " (显式列出允许注册的后端服务号，例如 [8])";
        return false;
    }
    out.allowedServices.clear();
    const std::vector<std::string> svcList = y.GetList("gateway.privateConfig.allowedServices");
    if (svcList.empty())
    {
        // 行内写法 `allowedServices: [8]` 会被本解析器当作**标量**存下来，GetList 拿到空列表。
        // 这正是"静默变成白名单为空"的坑，因此这里明确报错并给出正确写法。
        err = path + ": gateway.privateConfig.allowedServices must be a YAML list"
                     " (写成分行 '- 8' 形式；本解析器不支持行内 [8])";
        return false;
    }
    for (const std::string& s : svcList)
    {
        const long v = std::strtol(s.c_str(), nullptr, 10);
        if (v <= 0 || v > 255)
        {
            err = path + ": gateway.privateConfig.allowedServices has illegal value: " + s;
            return false;
        }
        out.allowedServices.push_back(static_cast<uint8_t>(v));
    }

    out.eventThreadNum = (uint32_t)y.GetUInt("gateway.eventThreadNum", out.eventThreadNum);

    // ---------------- 公网接入策略 ----------------
    out.clientToken = y.GetUInt("gateway.clientToken", out.clientToken);
    if (!ReadU8(y, "gateway.defaultServiceId", out.defaultServiceId, out.defaultServiceId, err))
    {
        err = path + ": " + err;
        return false;
    }
    out.maxClientSessions = (uint32_t)y.GetUInt("gateway.maxClientSessions", out.maxClientSessions);
    out.maxPendingRoutes = (uint32_t)y.GetUInt("gateway.maxPendingRoutes", out.maxPendingRoutes);
    out.routeTtlMs = (uint32_t)y.GetUInt("gateway.routeTtlMs", out.routeTtlMs);

    out.logLevel = y.GetStr("gateway.logLevel", out.logLevel);

    // 键名检查放在读取之后：到这里"代码认识的键"都已消费完，剩下的都是没人读的
    if (!CheckUnknownKeys(y, path, err))
        return false;

    const std::vector<std::string> verrs = out.Validate();
    if (!verrs.empty())
    {
        err = path + ": " + verrs.front();
        if (verrs.size() > 1)
            err += " (+" + std::to_string(verrs.size() - 1) + " more)";
        return false;
    }

    return true;
}

std::vector<std::string> GatewayConfig::Validate() const
{
    std::vector<std::string> errs;

    if (zoneId == 0)
        errs.push_back("gateway.zoneId must be >= 1");

    if (gatewayId == 0)
        errs.push_back("gateway.gatewayId must be >= 1");

    if (publicListenAddr.empty())
        errs.push_back("gateway.publicConfig.listenAddr must not be empty");

    if (privateListenAddr.empty())
        errs.push_back("gateway.privateConfig.listenAddr must not be empty");

    // 端口 0 是非法监听端口；uint16_t 天然不超过 65535，无需上界检查
    if (publicListenPort == 0)
        errs.push_back("gateway.publicConfig.listenPort must be in (0, 65535]");

    if (privateListenPort == 0)
        errs.push_back("gateway.privateConfig.listenPort must be in (0, 65535]");

    // 两个端口相同 = 后 bind 的监听一定失败；若用 SO_REUSEPORT 蒙对了，
    // 客户端连接就会落到内网链路的协议解析上 —— 必须直接拒绝这种配置。
    if (publicListenPort != 0 && publicListenPort == privateListenPort &&
        publicListenAddr == privateListenAddr)
        errs.push_back("gateway.publicConfig.listenPort and privateConfig.listenPort must differ"
                       " (客户端链路与内网链路必须分开监听)");

    // 线程池至少要 1 条 IO 线程；0 会导致 acceptor 分配不到 loop
    if (eventThreadNum == 0)
        errs.push_back("gateway.eventThreadNum must be >= 1");

    // 白名单：非空、不含"任意/网关自身"这两个特殊值
    if (allowedServices.empty())
        errs.push_back("gateway.privateConfig.allowedServices must not be empty"
                       " (至少列出可注册的后端服务号，例如 [8])");
    for (uint8_t v : allowedServices)
    {
        if (v == 0)
            errs.push_back("gateway.privateConfig.allowedServices must not contain 0 (kServiceAny)");
        if (v == 1)
            errs.push_back("gateway.privateConfig.allowedServices must not contain 1 (kGateway: 网关不能作为后端注册)");
    }

    if (defaultServiceId == 0)
        errs.push_back("gateway.defaultServiceId must be >= 1");

    // 默认目标必须在白名单里，否则客户端未显式绑定时每个请求都会 "service unavailable"
    bool defaultAllowed = false;
    for (uint8_t v : allowedServices)
    {
        if (v == defaultServiceId)
            defaultAllowed = true;
    }
    if (!defaultAllowed)
        errs.push_back("gateway.defaultServiceId must be listed in privateConfig.allowedServices");

    if (maxClientSessions == 0)
        errs.push_back("gateway.maxClientSessions must be >= 1");

    if (maxPendingRoutes == 0)
        errs.push_back("gateway.maxPendingRoutes must be >= 1");

    if (routeTtlMs == 0)
        errs.push_back("gateway.routeTtlMs must be >= 1");

    // 日志级别必须是已知值：拼错（例如 "warning"）如果不报错，
    // 就会静默落到默认级别，排障时"我明明开了 debug"却看不到任何调试日志
    if (logLevel != "debug" && logLevel != "info" && logLevel != "warn" && logLevel != "error")
        errs.push_back("gateway.logLevel must be one of debug/info/warn/error, got '" + logLevel + "'");

    return errs;
}
