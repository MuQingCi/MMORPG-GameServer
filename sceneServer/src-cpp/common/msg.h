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
    constexpr uint16_t LOGIN           = 1;    //登录
    constexpr uint16_t REGISTER        = 2;    //注册
    constexpr uint16_t MOVE            = 3;    //移动
    constexpr uint16_t BAG             = 4;    //背包
    constexpr uint16_t BATTLE          = 5;    //战斗
    constexpr uint16_t SHOPPINGMALL    = 6;    //商城
    constexpr uint16_t PLAYER          = 7;    //玩家
};


namespace method
{
    //登录
    constexpr uint16_t L_LOGIN_ACCOUNT      = 1;    //登录账号
    constexpr uint16_t L_LOGIN_PLAYER       = 2;    //登录角色

    //注册
    constexpr uint16_t R_REGISTER_ACCOUNT   = 1;    //注册账号
    constexpr uint16_t R_REGISTER_PLAYER    = 2;    //注册角色

    //移动
    constexpr uint16_t M_MOVE       = 1;

    //玩家
    constexpr uint16_t PLAYER_PUSH_STATE = 1;
}

struct MsgHead
{
    uint16_t msgType = MsgType::MSGTYPE_NONE;

    uint16_t src;   //发送线程id
    uint16_t dst;   //接收线程id

    //模块id,方法id--用于逻辑线程查路由并分发
    uint16_t modle;
    uint16_t method;
    
    uint64_t seq;      //请求序列号,原样回传
    uint32_t session;  //会话id
    uint64_t playerId; //玩家id
};

struct Msg
{
    MsgHead head;
    std::string body;
};

#endif