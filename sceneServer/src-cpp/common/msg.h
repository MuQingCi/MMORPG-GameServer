#ifndef CLEARMOON_COMMON_MSG_H
#define CLEARMOON_COMMON_MSG_H

#include <cstdint>
#include <string>
#include <chrono>
#include <sys/types.h>

/**
 * @brief 场景服内部消息通用格式
------------------------------------------------------------------
|消息类型|模块号|方法号|请求序列号|玩家ID|回投线程ID|上下文|  业务数据| 
------------------------------------------------------------------
 * 
 * @return int64_t 
 */

// 单调时钟毫秒
inline int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
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
    MSGTYPE_NONE       = 0,
    //连接类
    MSGTYPE_CONN_NEW   = 1,
    MSGTYPE_CONN_CLOSE = 2,
    MSGTYPE_CONN_DATA  = 3,
    
    //逻辑线程->Net
    MSGTYPE_SEND       = 4,
    MSGTYPE_CLOSE      = 5,

    //定时器
    MSGTYPE_TIMER_FIRE = 6,

    //数据库
    MSGTYPE_DB_TASK    = 7,
    MSGTYPE_DB_RESULT  = 8,

    //关闭
    MSGTYPE_SHUTDOWN   = 9,
    //热更消息
    MSGTYPE_RELOAD     = 10,

};

namespace Module
{
    constexpr uint16_t SYS             = 1;    //系统消息
    constexpr uint16_t TIMER           = 2;     //定时器
    constexpr uint16_t PLAYER          = 3;    //玩家模块
    // constexpr uint16_t MOVE            = 3;    //移动
    // constexpr uint16_t BAG             = 4;    //背包
    constexpr uint16_t ENEMY           = 4;    //敌人
    constexpr uint16_t BATTLE          = 5;    //战斗
    constexpr uint16_t SHOPPINGMALL    = 6;    //商城
    
};


namespace Method
{
    //------------------系统------------------
    //扫描空闲连接
    constexpr uint16_t SYS_SCAN_IDLE     =  1;
    constexpr uint16_t SYS_FLUSH         =  2;

    //------------------定时器----------------
    constexpr uint16_t TIMER_LUA_FIRE    =  1;

    //------------------玩家模块--------------
    //推送状态
    constexpr uint16_t PLAYER_PUSH_STATE =  1;
    //移动
    constexpr uint16_t PLAYER_MOVE       =  2;
    constexpr uint16_t PlAYER_PATH       =  3;
    constexpr uint16_t PLAYER_OPEN_BAG   =  10;

    //------------------敌人模块--------------
    constexpr uint16_t ENEMY_AI_TICK     =  1;
}

struct MsgHead
{
    uint16_t msgType = MsgType::MSGTYPE_NONE;

    //模块id,方法id--用于逻辑线程查路由并分发
    uint16_t Module      = 0;
    uint16_t Method      = 0;
    
    uint64_t seq         = 0;      //请求序列号,原样回传
    uint64_t session     = 0;  //会话id
    uint64_t playerId    = 0; //玩家id
    uint32_t srcWorkerId = 0; // 仅用于"结果回投原逻辑线程"，不是路由依据

    //上下文-根据消息类型决定ctx存的值
    uint64_t ctx         = 0;
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
};

// 便捷小工具
inline void PutU16(std::string& s, uint16_t v) {
  s.push_back(char(v & 0xFF));
  s.push_back(char((v >> 8) & 0xFF));
}
inline void PutU32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
}
inline void PutI64(std::string& s, int64_t v) {
  uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) s.push_back(char((u >> (8 * i)) & 0xFF));
}
inline uint16_t GetU16(const char* p) {
  return uint16_t(uint8_t(p[0]) | (uint8_t(p[1]) << 8));
}
inline uint32_t GetU32(const char* p) {
  return uint32_t(uint8_t(p[0]) | (uint8_t(p[1]) << 8) | (uint8_t(p[2]) << 16) |
                  (uint8_t(p[3]) << 24));
}
inline int64_t GetI64(const char* p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u |= uint64_t(uint8_t(p[i])) << (8 * i);
  return static_cast<int64_t>(u);
}

#endif