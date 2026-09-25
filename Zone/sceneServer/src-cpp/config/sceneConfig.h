#ifndef CLEARMOON_CONFIG_SCENECONFIG_H
#define CLEARMOON_CONFIG_SCENECONFIG_H

#include "config/zoneConfig.h"
#include "db/redisKey.h"
#include "logic/logicThread.h"
#include "net/net_server.h"

#include <cstdint>
#include <string>

/**
 * @brief 场景服配置（与 config/sceneConfig.yaml 一一对应）
 *
 * 只放**场景服特有**的东西；区服共享的部分**组合** ZoneConfig（`zone` 成员），
 * 而不是把字段再复制一份：
 *
 *   - 复制字段会导致"同一个配置项两份存储"：改哪一份生效不确定；
 *     日志 logName 都默认 "scene" 时，多个服务还会写同一个日志文件。
 *   - 组合之后，物理上不可能出现两份，用的时候写 `cfg.zone.db.mysql.host`，语义清晰。
 *
 * 注意：本结构**不再持有 zoneId**（区服身份的唯一权威是 ZoneConfig.zoneId）。
 * 网络握手需要的 `net.zoneId` / `net.sceneId` 与 Redis 命名空间，
 * 都由 `AttachZone()` 从"区服配置 + 本场景 sceneId"派生出来。
 */
struct SceneConfig
{
    // ---------------- 场景服本体 ----------------
    uint32_t sceneId = 1;                   // 场景编号（握手时上报网关）
    uint8_t  serviceId = 8;                 // ServerID::kScene_1
    uint32_t logicThreadNum = 4;            // 建议 2~4：每个 Lua VM 都加载全套脚本，内存 × N
    std::string luaDir = "./lua_script";
    uint64_t luaInstructionLimit = 10000000;  // 0 = 关闭 Lua 指令预算

    // ---------------- 逻辑线程 ----------------
    int64_t idleWaitMs = 5;            // 队列空时的阻塞等待上限
    int64_t flushIntervalMs = 30000;   // 定期快照间隔（防宕机丢数据）
    int64_t scanIdleMs = 10000;        // 空闲玩家扫描间隔

    // ---------------- 定时器 ----------------
    uint64_t timerTickMs = 50;
    uint32_t timerWheelSlots = 256;

    // ---------------- 网络（场景服自己的接入/上限参数）----------------
    NetConfig net;

    // ---------------- 区服共享配置（组合，不复制）----------------
    ZoneConfig zone;

    // 加载场景服自己那份配置；失败返回 false 并填充 err
    // （含：文件不存在 / 语法错误 / 迁移键 / 未知键 / 字段非法）
    static bool Load(const std::string& path, SceneConfig& out, std::string& err);

    /**
     * @brief 并入区服共享配置，并做一次整体校验
     *
     * 做三件事：
     *   1. 校验 ZoneConfig（zoneId / 日志名 / dbName 等）；
     *   2. 把派生字段同步成唯一值：net.zoneId / net.sceneId / net.serviceId、
     *      Redis 命名空间的 zoneId 与 sceneId；
     *   3. 校验场景服自身约束（logicThreadNum >= 1 等）。
     *
     * 调用方应在 Load 之后、构造 SceneServer 之前调用它（main.cc 已经这么做）。
     */
    bool AttachZone(const ZoneConfig& z, std::string& err);

    // 生成 LogicThread 需要的配置（含 Redis 命名空间）
    LogicThread::Config MakeLogicConfig() const;

    // Redis 命名空间（zone/scene 两段前缀）
    RedisNamespace RedisNs() const;
};

#endif

