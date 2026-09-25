#include "config/sceneConfig.h"

#include "base/yaml.h"
#include "log/logger.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
// 已迁移到 zoneConfig.yaml 的键。SceneConfig 里再出现一律报错：
// "同一个配置项写两处，迟早不一致"；而且之前正是这类残留导致
// "读了不存在的键 -> 静默回落默认值 -> 配置写了却没生效"。
struct MigratedKey
{
    const char* key;      // "SceneServer." 之后的部分
    const char* target;   // 现在应该写在哪儿
};

const MigratedKey kMigratedKeys[] = {
    {"zoneId", "ZoneServer.zoneId"},
    {"db", "ZoneServer.db.*"},
    {"gateways", "ZoneServer.gateways"},
    {"log", "ZoneServer.log.*"},
};

bool CheckMigratedKeys(const YamlLite& y, const std::string& path, std::string& err)
{
    for (const auto& m : kMigratedKeys)
    {
        const std::string full = std::string("SceneServer.") + m.key;
        // 容器键（db / log）本身没有值，Has() 看不到，必须用前缀匹配
        const bool present = y.Has(full) || y.HasPrefix(full + ".");
        if (present)
        {
            err = path + ": key '" + full + "' has moved to zoneConfig.yaml (" + m.target + ")";
            return false;
        }
    }
    return true;
}

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

LogicThread::Config SceneConfig::MakeLogicConfig() const
{
    LogicThread::Config c;
    c.luaDir = luaDir;
    c.idleWaitMs = idleWaitMs;
    c.flushIntervalMs = flushIntervalMs;
    c.scanIdleMs = scanIdleMs;
    c.luaInstructionLimit = luaInstructionLimit;
    c.redisNs = RedisNs();
    return c;
}

RedisNamespace SceneConfig::RedisNs() const
{
    RedisNamespace ns;
    ns.zoneId = zone.db.redis.zoneId;
    ns.sceneId = zone.db.redis.sceneId;
    return ns;
}

bool SceneConfig::AttachZone(const ZoneConfig& z, std::string& err)
{
    zone = z;

    // 1. 区服配置自身的语义校验
    const std::vector<std::string> zerrs = zone.Validate();
    if (!zerrs.empty())
    {
        err = "zone config invalid: " + zerrs.front();
        if (zerrs.size() > 1)
            err += " (+" + std::to_string(zerrs.size() - 1) + " more)";
        return false;
    }

    // 2. 派生字段同步（"一处配置，多处一致"）
    //    网络握手要上报 zoneId/sceneId/serviceId；Redis 命名空间要 zone+scene 两段前缀。
    zone.db.redis.zoneId = zone.zoneId;
    zone.db.redis.sceneId = sceneId;      // 场景服使用场景级 key 前缀

    net.serviceId = serviceId;
    net.zoneId = zone.zoneId;
    net.sceneId = sceneId;

    // 3. 场景服自身约束
    if (logicThreadNum == 0)
    {
        err = "scene config invalid: logicThreadNum must be >= 1";
        return false;
    }
    if (serviceId == 0)
    {
        err = "scene config invalid: serviceId must be >= 1";
        return false;
    }
    if (sceneId == 0)
    {
        err = "scene config invalid: sceneId must be >= 1";
        return false;
    }
    if (!net.listenEnable && zone.gateways.empty())
    {
        // 既不监听、也不反向连网关 → 这个进程没有任何入口，几乎肯定是配错了
        err = "scene config invalid: net.listenEnable=false and ZoneServer.gateways is empty "
              "(no way to receive traffic)";
        return false;
    }

    return true;
}

bool SceneConfig::Load(const std::string& path, SceneConfig& out, std::string& err)
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

    // ---------------- 场景服本体 ----------------
    out.sceneId = (uint32_t)y.GetUInt("SceneServer.sceneId", out.sceneId);
    out.serviceId = (uint8_t)y.GetUInt("SceneServer.serviceId", out.serviceId);
    out.logicThreadNum = (uint32_t)y.GetUInt("SceneServer.logicThreadNum", out.logicThreadNum);
    out.luaDir = y.GetStr("SceneServer.luaDir", out.luaDir);
    out.luaInstructionLimit = y.GetUInt("SceneServer.luaInstructionLimit", out.luaInstructionLimit);
    out.idleWaitMs = y.GetInt("SceneServer.idleWaitMs", out.idleWaitMs);
    out.flushIntervalMs = y.GetInt("SceneServer.flushIntervalMs", out.flushIntervalMs);
    out.scanIdleMs = y.GetInt("SceneServer.scanIdleMs", out.scanIdleMs);

    // ---------------- 网络 ----------------
    out.net.listenEnable = y.GetBool("SceneServer.net.listenEnable", out.net.listenEnable);
    out.net.listenAddr = y.GetStr("SceneServer.net.listenAddr", out.net.listenAddr);
    if (!y.GetPort("SceneServer.net.listenPort", out.net.listenPort, out.net.listenPort, err))
    {
        err = path + ": " + err;
        return false;
    }
    out.net.maxReadBufferBytes =
        (size_t)y.GetUInt("SceneServer.net.maxReadBufferBytes", out.net.maxReadBufferBytes);
    out.net.maxWriteBufferBytes =
        (size_t)y.GetUInt("SceneServer.net.maxWriteBufferBytes", out.net.maxWriteBufferBytes);
    out.net.maxFramesPerSec =
        (uint32_t)y.GetUInt("SceneServer.net.maxFramesPerSec", out.net.maxFramesPerSec);
    out.net.maxDecodeErrors =
        (uint32_t)y.GetUInt("SceneServer.net.maxDecodeErrors", out.net.maxDecodeErrors);

    // 本服务身份会写进出站帧头的 srcServiceID，必须与网关口中的 serviceId 一致
    out.net.serviceId = out.serviceId;
    out.net.sceneId = out.sceneId;
    // net.zoneId 来自 ZoneConfig，等 AttachZone() 再填（这里不读第二份 zoneId）

    // ---------------- 定时器 ----------------
    out.timerTickMs = y.GetUInt("SceneServer.timerTickMs", out.timerTickMs);
    out.timerWheelSlots = (uint32_t)y.GetUInt("SceneServer.timerWheelSlots", out.timerWheelSlots);

    // ---------------- 键名检查 ----------------
    // 先查"搬了家"（错误信息更具体），再查"拼错 / 没人读"
    if (!CheckMigratedKeys(y, path, err))
        return false;
    if (!CheckUnknownKeys(y, path, err))
        return false;

    if (out.logicThreadNum == 0)
    {
        err = path + ": logicThreadNum must be >= 1";
        return false;
    }

    return true;
}
