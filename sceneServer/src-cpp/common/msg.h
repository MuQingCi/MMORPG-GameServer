#ifndef CLEARMOON_COMMON_MSG_H
#define CLEARMOON_COMMON_MSG_H

#include <chrono>
#include <cstdint>
#include <string>
#include <sys/types.h>

/**
 * @brief 场景服内部消息通用格式
 * --------------------------------------------------------------------------------
 * |消息类型|模块号|方法号|请求序列号|会话ID|玩家ID|回投线程ID|Actor版本|上下文|业务数据|
 * --------------------------------------------------------------------------------
 *
 * 字段职责划分（越界使用是最容易出错的地方）：
 *   - msgType     : 决定这条消息走哪条处理分支（连接/发包/定时器/DB/热更）
 *   - Module      : 业务模块号；仅用于查路由表（不用于线程归属）
 *   - Method      : 业务方法号；仅用于查路由表
 *   - seq         : 客户端请求序列号，原样回传
 *   - session     : 会话 id；网络层用它定位连接（"往哪儿发字节"）
 *   - playerId    : 玩家 id；MsgBus 用它决定 Actor 归属（"谁的状态"）
 *   - srcWorkerId : 回投目标线程；仅用于"结果回到发起它的逻辑线程"，不是路由依据
 *   - epoch       : Actor 版本号；异步回调（DB/定时器）用它校验 Actor 生命周期
 *   - ctx         : 按 msgType 定义（Lua 定时器 id / Lua DB 回调 ctx / 对端服务 id）
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

enum MsgType : uint16_t
{
    MSGTYPE_NONE            = 0,
    // 连接类：由网络线程投给逻辑线程
    MSGTYPE_CONN_NEW        = 1,
    MSGTYPE_CONN_CLOSE      = 2,
    MSGTYPE_CONN_DATA       = 3,

    // 逻辑线程 -> 网络线程
    MSGTYPE_SEND            = 4,
    MSGTYPE_CLOSE           = 5,    // 逻辑层要求断开某会话（踢人/顶号）

    // 定时器：定时器线程 -> 逻辑线程
    MSGTYPE_TIMER_FIRE      = 6,

    // 数据库：MySQL 通道（Redis 必须独立通道，见设计文档 3.5）
    MSGTYPE_DB_TASK_MYSQL   = 7,
    MSGTYPE_DB_RESULT_MYSQL = 8,

    // 关闭
    MSGTYPE_SHUTDOWN        = 9,
    // 热更消息
    MSGTYPE_RELOAD          = 10,

    // 数据库：Redis 通道
    MSGTYPE_DB_TASK_REDIS   = 11,
    MSGTYPE_DB_RESULT_REDIS = 12,
};

// 便于日志输出
inline const char* MsgTypeName(uint16_t t)
{
    switch (t)
    {
        case MSGTYPE_NONE:            return "NONE";
        case MSGTYPE_CONN_NEW:        return "CONN_NEW";
        case MSGTYPE_CONN_CLOSE:      return "CONN_CLOSE";
        case MSGTYPE_CONN_DATA:       return "CONN_DATA";
        case MSGTYPE_SEND:            return "SEND";
        case MSGTYPE_CLOSE:           return "CLOSE";
        case MSGTYPE_TIMER_FIRE:      return "TIMER_FIRE";
        case MSGTYPE_DB_TASK_MYSQL:   return "DB_TASK_MYSQL";
        case MSGTYPE_DB_RESULT_MYSQL: return "DB_RESULT_MYSQL";
        case MSGTYPE_SHUTDOWN:        return "SHUTDOWN";
        case MSGTYPE_RELOAD:          return "RELOAD";
        case MSGTYPE_DB_TASK_REDIS:   return "DB_TASK_REDIS";
        case MSGTYPE_DB_RESULT_REDIS: return "DB_RESULT_REDIS";
        default:                      return "UNKNOWN";
    }
}

inline bool IsDbTask(uint16_t t)
{
    return t == MSGTYPE_DB_TASK_MYSQL || t == MSGTYPE_DB_TASK_REDIS;
}

inline bool IsDbResult(uint16_t t)
{
    return t == MSGTYPE_DB_RESULT_MYSQL || t == MSGTYPE_DB_RESULT_REDIS;
}

namespace Module
{
    constexpr uint16_t SYS          = 1;    // 系统消息（握手/心跳/内务）
    constexpr uint16_t TIMER        = 2;    // 定时器
    constexpr uint16_t PLAYER       = 3;    // 玩家模块
    constexpr uint16_t ENEMY        = 4;    // 敌人
    constexpr uint16_t BATTLE       = 5;    // 战斗
    constexpr uint16_t SHOPPINGMALL = 6;    // 商城
}

namespace Method
{
    //------------------系统------------------
    constexpr uint16_t SYS_HANDSHAKE     = 1;    // 服务间握手（反向连接后上报服务信息）
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
    uint16_t msgType = MsgType::MSGTYPE_NONE;

    // 模块id,方法id--用于逻辑线程查路由并分发
    uint16_t Module = 0;
    uint16_t Method = 0;

    uint64_t seq = 0;         // 请求序列号,原样回传
    uint64_t session = 0;     // 会话id
    uint64_t playerId = 0;    // 玩家id

    uint32_t srcWorkerId = 0; // 仅用于"结果回投原逻辑线程"，不是路由依据
    uint32_t epoch = 0;       // Actor 版本号：异步回调回到逻辑线程后先校验它

    // 上下文-根据消息类型决定ctx存的值
    uint64_t ctx = 0;

    // DB 通道内部分片索引：
    //   Redis 多实例分片时，投递前由 db.redis 绑定层按 key 哈希算出（见 db/redisKey.h），
    //   MsgBus 据此选择对应的 Redis 队列；MySQL 通道恒为 0。
    //   它**不是**路由依据（线程归属仍由 srcWorkerId/队列决定），只是队列选择器。
    uint16_t dbShard = 0;
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
