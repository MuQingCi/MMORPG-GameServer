#ifndef CLEARMOON_GATEWAY_CONFIG_H
#define CLEARMOON_GATEWAY_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 网关配置（与 gatewayServer/config/gatewayConfig.yaml 一一对应）
 *
 * 字段归属说明（阶段 0 的定位）：
 *   - 本结构只承载**网关自身**的接入参数（监听地址/端口、IO 线程数、网关编号）；
 *   - zoneId 目前独立配置；阶段 4 引入"反向连接 + 服务注册"后，
 *     网关的区服身份应与区服共享配置（`config/zoneConfig.yaml`）统一，届时再收敛；
 *   - 端口必须与 `config/zoneConfig.yaml` 的 `gateways` 列表一致，
 *     否则场景服的反向连接会连不上（阶段 0 任务 0.12 已对齐为 10000）。
 *
 * 加载策略（与 config/zoneConfig.h、config/sceneConfig.h 保持一致）：
 *   - YAML 子集解析（zone_common/common/yaml.h）；
 *   - **未知键检查**：yaml 里写了但代码不认识的键 → 直接报错并列出键名，
 *     避免"键名拼错 → 静默用默认值"这类最难排查的问题；
 *   - `Validate()` 做语义校验（端口范围、线程数下限等）。
 */
class GatewayConfig
{
public:
    uint32_t    zoneId = 1;                     // 所属区服
    uint32_t    gatewayId = 1;                  // 本网关在集群内的编号（用于会话/日志区分）
    std::string listenAddr = "127.0.0.1";       // 监听地址（对外服务时改 0.0.0.0）
    uint16_t    listenPort = 10000;             // 监听端口（必须与 zoneConfig.gateways 一致）
    uint32_t    eventThreadNum = 4;             // IO 线程数（>=1）

    // 加载：失败返回 false 并填充 err（文件不存在 / 语法错误 / 未知键 / 字段非法）
    static bool Load(const std::string& path, GatewayConfig& out, std::string& err);

    // 语义校验（可单独调用；Load 内部也会调用）
    std::vector<std::string> Validate() const;
};

#endif
