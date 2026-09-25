#include "config/zoneConfig.h"

#include "base/yaml.h"
#include "log/logger.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
// 从 "1.2.3.4:10000" 解析出 PeerConfig
bool ParsePeer(const std::string& spec, uint8_t serviceId, PeerConfig& out, std::string& err)
{
    const size_t colon = spec.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size())
    {
        err = "bad peer spec: " + spec + " (expect ip:port)";
        return false;
    }

    out.serviceId = serviceId;
    out.addr = spec.substr(0, colon);

    const int port = std::atoi(spec.c_str() + colon + 1);
    if (port <= 0 || port > 65535)
    {
        err = "bad peer port: " + spec;
        return false;
    }
    out.port = static_cast<uint16_t>(port);
    return true;
}

// 把"未被消费的键"整理成错误信息（见 YamlLite 顶部说明）
bool CheckUnknownKeys(const YamlLite& y, const std::string& path, std::string& err)
{
    const std::vector<std::string> unused = y.UnusedKeys();
    if (unused.empty())
        return true;

    std::string list;
    for (size_t i = 0; i < unused.size(); ++i)
        list += (i == 0 ? "" : ", ") + unused[i];

    err = path + ": unknown/unused config keys: " + list +
          "  (typo? or moved to another config file?)";
    return false;
}
}  // namespace

bool ZoneConfig::Load(const std::string& path, ZoneConfig& out, std::string& err)
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

    // ---------------- 区服身份 ----------------
    out.zoneId = (uint32_t)y.GetUInt("ZoneServer.zoneId", out.zoneId);

    // ---------------- 数据层 ----------------
    out.dbEnable = y.GetBool("ZoneServer.db.enable", out.dbEnable);

    out.db.mysql.host = y.GetStr("ZoneServer.db.mysql.host", out.db.mysql.host);
    if (!y.GetPort("ZoneServer.db.mysql.port", out.db.mysql.port, out.db.mysql.port, err))
    {
        err = path + ": " + err;
        return false;
    }
    out.db.mysql.user = y.GetStr("ZoneServer.db.mysql.user", out.db.mysql.user);
    out.db.mysql.password = y.GetStr("ZoneServer.db.mysql.password", out.db.mysql.password);
    out.db.mysql.dbName = y.GetStr("ZoneServer.db.mysql.dbName", out.db.mysql.dbName);
    out.db.mysql.charset = y.GetStr("ZoneServer.db.mysql.charset", out.db.mysql.charset);
    out.db.mysql.connectTimeoutSec =
        (uint32_t)y.GetUInt("ZoneServer.db.mysql.connectTimeoutSec", out.db.mysql.connectTimeoutSec);
    out.db.mysql.readTimeoutSec =
        (uint32_t)y.GetUInt("ZoneServer.db.mysql.readTimeoutSec", out.db.mysql.readTimeoutSec);

    out.db.redis.host = y.GetStr("ZoneServer.db.redis.host", out.db.redis.host);
    if (!y.GetPort("ZoneServer.db.redis.port", out.db.redis.port, out.db.redis.port, err))
    {
        err = path + ": " + err;
        return false;
    }
    out.db.redis.password = y.GetStr("ZoneServer.db.redis.password", out.db.redis.password);
    out.db.redis.timeoutMs = (uint32_t)y.GetUInt("ZoneServer.db.redis.timeoutMs", out.db.redis.timeoutMs);

    // Redis 多实例分片（可选）：shards: [ "127.0.0.1:6379", "127.0.0.1:6380" ]
    out.db.redis.shards = y.GetList("ZoneServer.db.redis.shards");

    // 命名空间：zoneId **从 ZoneConfig.zoneId 派生**，不允许在 redis 段再写一份
    // （同一个值写两处迟早不一致；真写了会被"未知键"检查拦下）
    out.db.redis.zoneId = out.zoneId;
    // sceneId 由具体服务在启动时填充（区服级服务保持 0 = 区服级 key）

    // ---------------- 网关集群 ----------------
    const std::vector<std::string> peers = y.GetList("ZoneServer.gateways");
    for (const auto& spec : peers)
    {
        PeerConfig pc;
        std::string perr;
        if (!ParsePeer(spec, ServerID::kGateway, pc, perr))
        {
            err = path + ": " + perr;
            return false;
        }
        out.gateways.push_back(pc);
    }

    // ---------------- 日志 ----------------
    out.logLevel = y.GetStr("ZoneServer.log.level", out.logLevel);
    out.logDir = y.GetStr("ZoneServer.log.dir", out.logDir);
    out.logName = y.GetStr("ZoneServer.log.name", out.logName);
    out.logRollSize = y.GetUInt("ZoneServer.log.rollSize", out.logRollSize);

    // ---------------- 键名拼写 / 搬家检查 ----------------
    // 到这里"代码认识的键"都已消费完，剩下的都是没人读的键 → 拼错或搬了家
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

std::vector<std::string> ZoneConfig::Validate() const
{
    std::vector<std::string> errs;

    if (zoneId == 0)
        errs.push_back("zoneId must be >= 1");

    // 日志名必须显式指定：多服务共用 zoneConfig，空值会让它们写同一个文件
    if (logName.empty())
        errs.push_back("log.name must be set (scene/global/chat/...) to avoid shared log files");

    if (dbEnable)
    {
        // ★ 这条就是"配置写了却不生效"的守门人：
        //   dbName 为空时 MySQL 连不上，而启动日志又会打印 dbEnable=1，极具迷惑性
        if (db.mysql.dbName.empty())
            errs.push_back("db.mysql.dbName must be set when db.enable is true");
        if (db.mysql.user.empty())
            errs.push_back("db.mysql.user must be set when db.enable is true");

        for (size_t i = 0; i < db.redis.shards.size(); ++i)
        {
            std::string h;
            uint16_t p = 0;
            if (!db.redis.EndpointOf(i, h, p))
                errs.push_back("db.redis.shards[" + std::to_string(i) + "] is not 'ip:port': " +
                               db.redis.shards[i]);
        }
    }

    return errs;
}
