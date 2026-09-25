#ifndef CLEARMOON_CONFIG_ZONECONFIG_H
#define CLEARMOON_CONFIG_ZONECONFIG_H

#include "db/dbConfig.h"
#include "net/net_server.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 区服共享配置（与 config/zoneConfig.yaml 一一对应）
 *
 * 归属判断依据：**"这个配置是不是整个区服共享的"**。
 * 场景服 / 全局服 / 聊天服都读同一份 zoneConfig，因此这里放：
 *
 *   - zoneId    : 区服身份（**唯一权威**：Redis 命名空间也从它派生）
 *   - db        : 区服共享的 MySQL + Redis（区服内不做读写分离，只有主库）
 *   - gateways  : 区服网关集群（三种服务都要反向连同一批）
 *   - log       : 区服统一日志策略
 *
 * 场景服特有的项（sceneId/serviceId/logicThreadNum/luaDir/net/timer/...）在 SceneConfig 里。
 *
 * 注意：`zoneId` **只在这里出现**。历史上同一个值既写在 zoneConfig 又写在 sceneConfig，
 * 迟早会不一致；现在 SceneConfig 不再持有 zoneId（它的网络/Redis 命名空间从本结构派生）。
 */
struct ZoneConfig
{
    // 默认 0 = "未配置"：Validate() 会拒绝它。
    // 不给一个看起来正常的默认值（例如 1）是刻意的 —— 否则漏配 zoneId 会静默变成 1 号区服，
    // 而 1 号区服的 Redis key 前缀也会跟着错。
    uint32_t zoneId = 0;

    // ---------------- 数据层（区服共享）----------------
    bool     dbEnable = true;
    DbConfig db;

    // ---------------- 网关集群（各服务都要反向连）----------------
    std::vector<PeerConfig> gateways;

    // ---------------- 日志（区服统一策略）----------------
    std::string logLevel = "info";
    std::string logDir   = "./logs";
    // 默认留空：不同服务共用 zoneConfig 时必须各自显式指定，
    // 否则三个服务会往同一个 scene.log 里写（Validate 会拒绝空值）
    std::string logName;
    uint64_t logRollSize = 64 * 1024 * 1024;

    // 加载：失败返回 false 并填充 err（文件不存在 / 语法错误 / 未知键 / 迁移键 / 字段非法）
    static bool Load(const std::string& path, ZoneConfig& out, std::string& err);

    // 语义校验（在 Load 之后、使用之前调用；SceneConfig::AttachZone 也会调用它）
    std::vector<std::string> Validate() const;
};

#endif
