#ifndef CLEARMOON_GATEWAY_CONFIG_H
#define CLEARMOON_GATEWAY_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 网关配置（与 gatewayServer/config/gatewayConfig.yaml 一一对应）
 *
 * **两个监听端口是"区分两类数据"的第一层**（第二层是协议魔数，见 gatewayDispatcher.h）：
 *   - publicConfig  : 公网监听，只接客户端（ClientFrame，魔数 0xC1EB）
 *   - privateConfig : 区服内网监听，只接区服内其它服务器（GateFrame，魔数 0xC1EA）
 * 两者必须配置成不同端口（Validate 会拒绝同端口，否则后 bind 的会失败、或两类连接串流）。
 *
 * privateConfig.allowedServices 是 后端服务号白名单，必须显式写在配置里：
 *   - 只有白名单内的 serviceId 才能通过握手注册（否则场景服可以被任意进程冒充）；
 *   - 它落在"区服内网"这道信任边界上，因此**不拿默认值兜底**：
 *     配置里缺这个键 → 启动即失败，而不是静默放行。
 *
 * 加载策略（与 config/zoneConfig、sceneConfig 保持一致）：
 *   - YAML 子集解析（zone_common/base/yaml.h）；
 *   - **未知键检查**：yaml 里写了但代码不认识的键 → 直接报错并列出键名；
 *   - `Validate()` 做语义校验（端口范围、两端口不同、白名单非空等）。
 */
class GatewayConfig
{
public:
    uint32_t    zoneId = 1;                     // 所属区服（后端握手上报的 zoneId 必须与它一致）
    uint32_t    gatewayId = 1;                  // 本网关在集群内的编号（用于会话/日志区分）

    // ---- 公网（客户端）监听 ----
    std::string publicListenAddr = "127.0.0.1";
    uint16_t    publicListenPort = 10000;

    // ---- 内网（区服内其它服务器）监听 ----
    std::string privateListenAddr = "127.0.0.1";
    uint16_t    privateListenPort = 13145;
    std::vector<uint8_t> allowedServices{8};    // 允许注册的后端服务号白名单（8 = SCENE_1）

    uint32_t    eventThreadNum = 4;             // IO 线程数（>=1）

    // ---- 公网接入策略 ----
    uint64_t    clientToken = 0;                // 客户端最小鉴权票据；0 = 关闭校验（仅联调）
    uint8_t     defaultServiceId = 8;           // 客户端未绑定后端服务时的默认目标
    uint32_t    maxClientSessions = 4096;       // 公网连接数上限
    uint32_t    maxPendingRoutes = 10000;       // 在途请求上限（有界，防内存无界增长）
    uint32_t    routeTtlMs = 15000;             // 回程路由 TTL：后端不响应时回收

    // 日志级别：debug / info / warn / error
    // （网关原先写死 INFO，排障时只能改代码重编；这里先做成配置项，
    //   与 zoneConfig 的 log.level 统一是阶段 4 的收尾项）
    std::string logLevel = "info";

    // 加载：失败返回 false 并填充 err（文件不存在 / 语法错误 / 未知键 / 缺白名单 / 字段非法）
    static bool Load(const std::string& path, GatewayConfig& out, std::string& err);

    // 语义校验（可单独调用；Load 内部也会调用）
    std::vector<std::string> Validate() const;
};

#endif
