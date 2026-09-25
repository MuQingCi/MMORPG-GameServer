#ifndef CLEARMOON_DB_REDISAPI_H
#define CLEARMOON_DB_REDISAPI_H

#include <cstdarg>
#include <sys/time.h>

/**
 * @brief Redis 客户端符号统一层
 *
 * 现状：系统里可能装的是 hiredis（redis* 前缀），也可能是它的社区分支 valkey
 *       （valkey* 前缀 + VALKEY_REPLY_* 常量）。两者的 API 形状完全一致，
 *       差别只在符号名。
 *
 * 做法：只在这一层做别名映射，业务代码统一写 redis* 名字。
 *       这样换客户端不需要改任何业务逻辑，也不会把两套名字散落到代码各处。
 *
 * 编译期开关（由 CMake 决定）：
 *   SCENE_USE_VALKEY  1 = 用 libvalkey，0 = 用 hiredis
 */
#if defined(SCENE_USE_VALKEY) && SCENE_USE_VALKEY
#include <valkey/valkey.h>

using redisContext = valkeyContext;
using redisReply = valkeyReply;

#define REDIS_REPLY_STRING  VALKEY_REPLY_STRING
#define REDIS_REPLY_ARRAY   VALKEY_REPLY_ARRAY
#define REDIS_REPLY_INTEGER VALKEY_REPLY_INTEGER
#define REDIS_REPLY_NIL     VALKEY_REPLY_NIL
#define REDIS_REPLY_STATUS  VALKEY_REPLY_STATUS
#define REDIS_REPLY_ERROR   VALKEY_REPLY_ERROR

inline redisContext* redisConnectWithTimeout(const char* ip, int port, const struct timeval tv)
{
    return ::valkeyConnectWithTimeout(ip, port, tv);
}
inline int redisSetTimeout(redisContext* c, const struct timeval tv)
{
    return ::valkeySetTimeout(c, tv);
}
inline void redisFree(redisContext* c)
{
    ::valkeyFree(c);
}
inline void* redisCommand(redisContext* c, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    void* r = ::valkeyvCommand(c, fmt, ap);
    va_end(ap);
    return r;
}
inline void* redisCommandArgv(redisContext* c, int argc, const char** argv, const size_t* argvlen)
{
    return ::valkeyCommandArgv(c, argc, argv, argvlen);
}
#else
#include <hiredis/hiredis.h>
#endif

#endif
