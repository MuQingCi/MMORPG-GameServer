#include "gatewayConfig.h"

#include "common/yaml.h"

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

    err = path + ": unknown/unused config keys: " + list + "  (typo?)";
    return false;
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
    out.listenAddr = y.GetStr("gateway.listenAddr", out.listenAddr);
    out.eventThreadNum = (uint32_t)y.GetUInt("gateway.eventThreadNum", out.eventThreadNum);

    // 端口范围校验在 GetPort 内部完成（在窄化之前判 (0, 65535]，
    // 避免 (uint16_t)GetUInt 把 70000 静默截断成 4464）
    if (!y.GetPort("gateway.listenPort", out.listenPort, out.listenPort, err))
    {
        err = path + ": " + err;
        return false;
    }

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

    if (listenAddr.empty())
        errs.push_back("gateway.listenAddr must not be empty");

    // 端口 0 是非法监听端口；uint16_t 天然不超过 65535，无需上界检查
    if (listenPort == 0)
        errs.push_back("gateway.listenPort must be in (0, 65535]");

    // 线程池至少要 1 条 IO 线程；0 会导致 acceptor 分配不到 loop
    if (eventThreadNum == 0)
        errs.push_back("gateway.eventThreadNum must be >= 1");

    return errs;
}
