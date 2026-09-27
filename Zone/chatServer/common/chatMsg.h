#ifndef CLEARMOON_CHATSERVER_COMMON_CHATMSG_H
#define CLEARMOON_CHATSERVER_COMMON_CHATMSG_H

#include "base/proto.h"   // 服务间 SYS 通道常量（握手方法号由公共协议层唯一定义）

#include <chrono>
#include <cstdint>
#include <string>
#include <sys/types.h>

static_assert(SysModule::kSYS == 1, "SYS 通道号被改动：网关/场景服/全局服必须一致");

/**
 * @brief 聊天服内部消息通用格式
 * --------------------------------------------------------------
 * |2B魔数|2B版本|2B模块|2B方法|8B源玩家ID|8B目的玩家ID|业务数据body|
 * --------------------------------------------------------------
 *
 * 字段职责划分（越界使用是最容易出错的地方）：
     - Magic       : 标识该消息是否属于当前服务器关注的消息
     - Version     : 标识该消息发送时处于的版本号
 *   - Module      : 标识该消息调用哪个模块方法——如：GM模块、Player模块
 *   - Method      : 标识该消息调用什么方法——如：私聊、广播等
 *   - srcPlayerId : 源玩家 Id
 *   - dstPlayerId : 目标玩家Id
 *   - body        : 聊天消息——暂定为单一字符串String
 */

// 单调时钟毫秒（用于耗时统计、心跳、idle 判定，不受系统时间调整影响）
inline int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

enum ThreadId : uint16_t
{
    THREAD_NET   = 1,
    THREAD_LOGIC = 2,
    THREAD_TIMER = 3,
    THREAD_DB    = 4,
};


namespace Module
{
    constexpr uint16_t SYS          = SysModule::kSYS;  // 系统消息（握手/心跳/内务）
    constexpr uint16_t TIMER        = 2;    // 定时器
    constexpr uint16_t PLAYER       = 3;    // 玩家模块
    constexpr uint16_t ENEMY        = 4;    // 敌人
    constexpr uint16_t BATTLE       = 5;    // 战斗
    constexpr uint16_t SHOPPINGMALL = 6;    // 商城
}

namespace Method
{
    //------------------系统------------------
    // 服务间握手（反向连接后上报服务信息）：号由公共协议层定义，
    // 场景服/网关/全局服/聊天服都引用它，避免"一侧改号、另一侧静默不解帧"。
    constexpr uint16_t SYS_HANDSHAKE     = SysMethod::kHandshake;
    constexpr uint16_t SYS_SCAN_IDLE     = 2;    // 扫描空闲连接
    constexpr uint16_t SYS_FLUSH         = 3;    // 周期落盘
    constexpr uint16_t SYS_RET_TIP       = 4;    // 通用提示(S->C)，对应 gs.RetTip

    //------------------定时器------------------
    constexpr uint16_t TIMER_LUA_FIRE    = 1;    // Lua 定时器到期的内部路由

    //------------------玩家模块--------------
    constexpr uint16_t PLAYER_PUSH_STATE = 1;    // 推送状态
    constexpr uint16_t PLAYER_MOVE       = 2;    // 移动
    constexpr uint16_t PLAYER_PATH       = 3;    // 寻路
    constexpr uint16_t PLAYER_OPEN_BAG   = 10;   // 打开背包

    //------------------敌人模块--------------
    constexpr uint16_t ENEMY_AI_TICK     = 1;
}

struct MsgHead
{
    Module module;
};

struct Msg
{
    MsgHead head;
    std::string body;
};

struct TimerOp // 定时器线程专用，不塞进 MsgBus
{
    enum Kind { ADD, CANCEL };
    Kind kind              = Kind::ADD;
    uint64_t timerId       = 0;
    uint64_t ownerWorkerId = 0;   // 到期后回投哪个逻辑线程
    uint64_t intervalMs    = 0;
    bool repeat            = false;
    uint16_t Module = 0, Method = 0;
    uint64_t playerId = 0, session = 0, seq = 0;
    uint32_t epoch = 0;           // 发起时刻的 Actor 版本号，原样透传给 TIMER_FIRE
};

// 便捷小工具：宿主序小端打包（仅用于进程内/DB 通道的轻量编解码）
inline void PutU16(std::string& s, uint16_t v)
{
    s.push_back(char(v & 0xFF));
    s.push_back(char((v >> 8) & 0xFF));
}
inline void PutU32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
}
inline void PutI64(std::string& s, int64_t v)
{
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) s.push_back(char((u >> (8 * i)) & 0xFF));
}
inline uint16_t GetU16(const char* p)
{
    return uint16_t(uint8_t(p[0]) | (uint8_t(p[1]) << 8));
}
inline uint32_t GetU32(const char* p)
{
    return uint32_t(uint8_t(p[0]) | (uint8_t(p[1]) << 8) | (uint8_t(p[2]) << 16) |
                    (uint8_t(p[3]) << 24));
}
inline int64_t GetI64(const char* p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= uint64_t(uint8_t(p[i])) << (8 * i);
    return static_cast<int64_t>(u);
}

#endif
