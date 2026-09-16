#ifndef CLEARMOON_COMMON_MSG_H
#define CLEARMOON_COMMON_MSG_H

#include <cstdint>
#include <string>
#include <chrono>
#include <sys/types.h>

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
    MSGTYPE_SHUTDOWN   = 9
};

namespace Modle
{
    constexpr uint16_t MOVE            = 3;    //移动
    constexpr uint16_t BAG             = 4;    //背包
    constexpr uint16_t BATTLE          = 5;    //战斗
    constexpr uint16_t SHOPPINGMALL    = 6;    //商城
    constexpr uint16_t PLAYER          = 7;    //玩家模块
};


namespace method
{
    //移动
    constexpr uint16_t M_MOVE       = 1;

    //玩家
    constexpr uint16_t PLAYER_PUSH_STATE = 1;
}

struct MsgHead
{
    uint16_t msgType = MsgType::MSGTYPE_NONE;

    //模块id,方法id--用于逻辑线程查路由并分发
    uint16_t modle;
    uint16_t method;
    
    uint64_t seq;      //请求序列号,原样回传
    uint64_t session;  //会话id
    uint64_t playerId; //玩家id
    uint32_t srcWorkerId; // 仅用于"结果回投原逻辑线程"，不是路由依据
};

struct Msg
{
    MsgHead head;
    std::string body;
};

struct TimerOp // 定时器线程专用，不塞进 MsgBus
{          
    enum Kind { ADD, CANCEL } kind;
    uint64_t timerId;
    uint64_t ownerWorkerId;   // 到期后回投哪个逻辑线程
    uint64_t intervalMs;
    bool repeat;
    uint16_t module, method;
    uint64_t playerId, session, seq;
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