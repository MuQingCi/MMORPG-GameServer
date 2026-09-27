#include "common/msg.h"
#include "common/msgBus.h"
#include "db/dbValue.h"
#include "db/redisKey.h"
#include "lua/luaApiCtx.h"
#include "lua/luaEnv.h"
#include "lua/lua_register.h"
#include "test_util.h"

#include <lua.hpp>
#include <string>

// ---------------------------------------------------------------------------
// 绑定层集成测试：真正走 db.redis 的 C 函数，验证
//   1) 脚本裸写 key 被拒绝（同步返回 (false, err)，不会投递任务）；
//   2) 用 db.key.* 构造的 key 被接受，并按 key 哈希投到正确的 Redis 分片队列；
//   3) db.key.zone/scene/player 的字符串与 RedisNamespace 完全一致。
// ---------------------------------------------------------------------------
namespace
{
struct LuaFixture
{
    LuaApiCtx ctx;
    MsgBus bus{1, 4};      // 4 个 Redis 分片
    LuaEnv env;            // 不 Init：只需要它提供 RegisterDbAck(ctx) 的登记能力

    LuaFixture()
    {
        ctx.bus = &bus;
        ctx.env = &env;
        ctx.workerId = 0;
        ctx.redisNs.zoneId = 3;
        ctx.redisNs.sceneId = 7;
    }
};

// 跑一段 Lua 代码，返回是否执行成功
bool RunChunk(lua_State* L, const char* code)
{
    if (luaL_loadstring(L, code) != 0)
    {
        std::fprintf(stderr, "lua load fail: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != 0)
    {
        std::fprintf(stderr, "lua run fail: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool GetGlobalBool(lua_State* L, const char* name)
{
    lua_getglobal(L, name);
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

std::string GetGlobalStr(lua_State* L, const char* name)
{
    lua_getglobal(L, name);
    const char* s = lua_tostring(L, -1);
    std::string out = s != nullptr ? s : "";
    lua_pop(L, 1);
    return out;
}
}  // namespace

TEST(LuaRedisRejectsRawKey)
{
    LuaFixture fx;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    CHECK(luaapi::RegisterAll(L, &fx.ctx));

    // 裸写 key：必须同步失败，且没有任何任务被投递
    CHECK(RunChunk(L, "ok, err = db.redis({'SET', 'bag:1001', '1'}, 'demo.on_ack')"));
    CHECK(!GetGlobalBool(L, "ok"));
    CHECK(GetGlobalStr(L, "err").find("outside namespace") != std::string::npos);

    Msg m;
    for (uint16_t shard = 0; shard < 4; ++shard)
        CHECK(!fx.bus.tryPopDbRedis(shard, m));

    lua_close(L);
}

TEST(LuaRedisNamespacedKeyRoutedToShard)
{
    LuaFixture fx;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    CHECK(luaapi::RegisterAll(L, &fx.ctx));

    // 用 db.key.* 构造 key：应当成功，并且任务落到 key 哈希对应的分片
    CHECK(RunChunk(
        L,
        "local k = db.key.scene('bag')\n"
        "ok, err = db.redis({'HSET', k, 'f', 'v'}, 'demo.on_ack')\n"
        "k_zone   = db.key.zone('rank')\n"
        "k_scene  = k\n"
        "k_player = db.key.player(1001, 'bag')\n"));

    CHECK(GetGlobalBool(L, "ok"));
    CHECK(GetGlobalStr(L, "err").empty());

    // db.key.* 的字符串必须与 RedisNamespace 完全一致
    const RedisNamespace ns = fx.ctx.redisNs;
    CHECK(GetGlobalStr(L, "k_zone") == ns.ZoneKey("rank"));
    CHECK(GetGlobalStr(L, "k_scene") == ns.SceneKey("bag"));
    CHECK(GetGlobalStr(L, "k_player") == ns.PlayerKey(1001, "bag"));

    // 分片必须等于"按 key 哈希"的结果
    const std::string sceneKey = ns.SceneKey("bag");
    const size_t expectShard = RedisShardOf(sceneKey, fx.bus.redisShardCount());

    Msg m;
    CHECK(fx.bus.tryPopDbRedis(static_cast<uint16_t>(expectShard), m));
    CHECK_EQ(m.head.dbShard, static_cast<uint16_t>(expectShard));
    CHECK_EQ(m.head.msgType, (uint16_t)MsgType::MSGTYPE_DB_TASK_REDIS);
    CHECK_EQ(m.head.srcWorkerId, (uint32_t)0);     // 结果要回到发起它的线程
    CHECK(m.head.ctx != 0);                        // 有 Lua 回调 ctx

    dbv::DbTask task;
    CHECK(dbv::DbTask::Decode(m.body, task));
    CHECK_EQ(task.kind, (uint8_t)3);
    CHECK_EQ(task.args.size(), (size_t)4);
    if (task.args.size() == 4)
    {
        CHECK(task.args[0] == "HSET");
        CHECK(task.args[1] == sceneKey);
        CHECK(task.args[2] == "f");
        CHECK(task.args[3] == "v");
    }

    // 其它分片不应该收到这条任务
    for (uint16_t shard = 0; shard < fx.bus.redisShardCount(); ++shard)
    {
        if (shard == expectShard)
            continue;
        CHECK(!fx.bus.tryPopDbRedis(shard, m));
    }

    lua_close(L);
}
