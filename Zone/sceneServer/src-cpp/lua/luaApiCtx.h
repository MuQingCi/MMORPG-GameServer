#ifndef CLEARMOON_LUA_LUAAPICTX_H
#define CLEARMOON_LUA_LUAAPICTX_H

#include "db/redisKey.h"

#include <cstdint>

class LuaEnv;
class lua_State;
class MsgBus;
class PlayerManager;
class ITimerService;
struct TimerOp;

/**
 * @brief Lua C API 的线程上下文（每个逻辑线程一份，注册进该线程 lua_State 的 registry）
 *
 * 为什么必须有它：
 *   旧代码的绑定层写的是 LuaEnv::I() / PlayerMgr::I() / NetServer::SendPacket(...) ——
 *   全局单例 + 直接摸别的线程的数据结构。这在 per-thread lua_State 的前提下根本不可能成立：
 *   要么拿不到"当前线程"的 state，要么跨线程访问网络层/玩家数据（数据竞争）。
 *
 * 现在：绑定函数只通过本结构拿到 本线程 能安全访问的对象：
 *   - bus        : 跨线程唯一合法通道（sendToNet / sendToDb / sendToWorker）
 *   - players    : 本 worker 的玩家分片（同线程直调安全）
 *   - timer      : 只做"入队 + 唤醒"的定时器服务
 *   - env        : 本线程的 LuaEnv（回调登记、定时器登记）
 *
 * currentPlayerId / currentSession / currentEpoch 由 DispatchC2S 在调用 Lua 前写入，
 * 供 net.send / db.* 自动带上"我是谁"，避免脚本自己传错身份。
 */
struct LuaApiCtx
{
    LuaEnv*        env = nullptr;
    MsgBus*        bus = nullptr;
    PlayerManager* players = nullptr;
    ITimerService* timer = nullptr;

    lua_State*      state = nullptr;    //协程句柄

    uint32_t workerId = 0;

    // Redis 命名空间：db.redis / db.key.* 用它补前缀与校验 key
    // （脚本不允许裸写 key，见 db/redisKey.h::ValidateRedisCommand）
    RedisNamespace redisNs;

    // 当前正在处理的消息上下文（进 Lua 前设置，出 Lua 后清零）
    uint64_t currentPlayerId = 0;
    uint64_t currentSession = 0;
    uint64_t currentSeq = 0;
    uint32_t currentEpoch = 0;
};

#endif
