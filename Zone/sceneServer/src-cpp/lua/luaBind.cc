// ============================================================================
// luaBind.cc —— C++ API 绑定到 Lua 层（**每线程上下文版**）
//
// 设计要点（对应建议书 D6 / E4 / E5）：
//   1. 不再有 LuaEnv::I() / PlayerMgr::I() / NetServer::SendPacket 这类全局单例直调：
//      绑定函数只通过 registry 里的 LuaApiCtx* 拿到**本线程**能安全访问的对象；
//   2. net.send 是**异步**的：只把 Msg 投进 MsgBus（入队 + 唤醒网络线程），
//      返回值代表"已入队"而不是"已发出"。跨线程碰 NetServer 的写缓冲就是数据竞争；
//   3. 全程不使用 luaL_error：它会 longjmp，跳过 C++ 栈上对象的析构
//      （std::string / Message 等）。脚本作者一定会用 pcall 包住调用，
//      错误被 Lua 接住 -> C++ 侧对象泄漏。统一改成返回 (ok, err) 元组。
//   4. 不向 Lua 暴露 SQL 模板白名单：语义化 API 由 C++ DAO 提供，
//      SQL 只出现在 C++ 里（可缓存、可参数化、可批量合并写）。
// ============================================================================
#include "lua/luaApiCtx.h"
#include "lua/luaEnv.h"
#include "lua/lua_register.h"

#include "common/msg.h"
#include "common/msgBus.h"
#include "db/dbValue.h"
#include "db/redisKey.h"
#include "log/logger.h"
#include "player/playerManager.h"
#include "proto/pb_lua.h"
#include "routeTable.h"

#include <lua.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
// ---------------- 上下文与返回值辅助 ----------------
LuaApiCtx* Ctx(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, luaapi::kApiCtxKey);
    LuaApiCtx* ctx = static_cast<LuaApiCtx*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ctx;
}

// 业务失败：返回 (false, err)，而不是 luaL_error（见文件头第 3 条）
int PushFail(lua_State* L, const char* err)
{
    lua_pushboolean(L, 0);
    lua_pushstring(L, err ? err : "error");
    return 2;
}

int PushOk(lua_State* L)
{
    lua_pushboolean(L, 1);
    return 1;
}

bool ReadStrArray(lua_State* L, int idx, std::vector<std::string>& out)
{
    if (lua_type(L, idx) != LUA_TTABLE)
        return false;

    const size_t n = lua_rawlen(L, idx);
    out.reserve(n);
    for (size_t i = 1; i <= n; ++i)
    {
        lua_rawgeti(L, idx, (lua_Integer)i);
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        if (s != nullptr)
            out.emplace_back(s, len);
        else
            out.emplace_back();
        lua_pop(L, 1);
    }
    return true;
}
}  // namespace

namespace luaapi
{

// ==================== net ====================
// net.send(session, "SCMove", {x=1,y=2}, seq?, pid?) -> true | (false, err)
static int l_net_send(lua_State* L)
{
    LuaApiCtx* c = Ctx(L);
    if (c == nullptr || c->bus == nullptr)
        return PushFail(L, "no api context");

    const uint64_t session = (uint64_t)luaL_optinteger(L, 1, (lua_Integer)c->currentSession);
    const char* msgName = luaL_optstring(L, 2, nullptr);
    if (session == 0 || msgName == nullptr)
        return PushFail(L, "net.send(session, msgName, table)");

    if (lua_type(L, 3) != LUA_TTABLE)
        return PushFail(L, "arg3 must be table");

    // 出站路由：用应答消息名找到 Module/Method（routeTable 已校验该名字存在）
    const RouteTable::Route* route = RouteTable::FindByTypeName(msgName);
    if (route == nullptr)
        return PushFail(L, "unknown msg name");

    std::string body;
    if (!pbl::TableToBody(L, 3, route->ack_type, body))
        return PushFail(L, "table->msg convert fail");

    Msg m;
    m.head.msgType  = MsgType::MSGTYPE_SEND;
    m.head.Module   = route->Module;
    m.head.Method   = route->Method;
    m.head.seq      = (uint64_t)luaL_optinteger(L, 4, (lua_Integer)c->currentSeq);
    m.head.playerId = (uint64_t)luaL_optinteger(L, 5, (lua_Integer)c->currentPlayerId);
    m.head.session  = session;
    m.body          = std::move(body);

    // 只入队 + 唤醒网络线程；实际发送由网络线程完成（异步语义）
    if (!c->bus->sendToNet(std::move(m)))
        return PushFail(L, "net queue full");

    return PushOk(L);
}

// ==================== log ====================
static int l_log_info(lua_State* L)
{
    const char* s = luaL_optstring(L, 1, "");
    LOG_INFO << "[lua] " << s;
    return 0;
}

static int l_log_warn(lua_State* L)
{
    const char* s = luaL_optstring(L, 1, "");
    LOG_WARNING << "[lua] " << s;
    return 0;
}

static int l_log_error(lua_State* L)
{
    const char* s = luaL_optstring(L, 1, "");
    LOG_ERROR << "[lua] " << s;
    return 0;
}

// ==================== db ====================
// kind: 1 = MySQL select, 2 = MySQL exec, 3 = Redis 命令（独立通道，不与 MySQL 混队）
static int SendDb(lua_State* L, uint8_t kind)
{
    LuaApiCtx* c = Ctx(L);
    if (c == nullptr || c->bus == nullptr || c->env == nullptr)
        return PushFail(L, "no api context");

    dbv::DbTask task;
    task.kind = kind;
    std::string cb;
    uint16_t shard = 0;

    if (kind == 3)
    {
        // db.redis({"HSET", db.key.scene("bag"), "f", "v"}, "module.on_ack")
        if (!ReadStrArray(L, 1, task.args) || task.args.empty())
            return PushFail(L, "db.redis(args[], cb)");
        cb = luaL_optstring(L, 2, "");

        // ★ Redis 的 key 隔离与分片都在这里定死：
        //   - key 必须落在本区服命名空间内（脚本不能裸写 key，否则跨场景互相覆盖）；
        //   - 分片按 key 稳定哈希选择（同一 key 永远同一实例）；
        //   - 多 key 命令如果跨分片直接拒绝（Redis 无法跨实例完成一次多键操作）。
        size_t shardIdx = 0;
        std::string verr;
        const size_t shardCount = c->bus->redisShardCount();
        if (!ValidateRedisCommand(task.args, c->redisNs, shardCount, shardIdx, verr))
            return PushFail(L, verr.c_str());
        shard = static_cast<uint16_t>(shardIdx);
    }
    else
    {
        // db.query("SELECT ... WHERE id=?", {1}, "module.on_ack")
        const char* sql = luaL_optstring(L, 1, nullptr);
        if (sql == nullptr || sql[0] == '\0')
            return PushFail(L, "db.query(sql, params?, cb)");
        task.args.emplace_back(sql);
        if (lua_istable(L, 2))
        {
            std::vector<std::string> params;
            if (!ReadStrArray(L, 2, params))
                return PushFail(L, "params must be string array");
            task.args.insert(task.args.end(), params.begin(), params.end());
        }
        cb = luaL_optstring(L, 3, "");
    }

    if (cb.empty())
        return PushFail(L, "callback required");

    Msg m;
    m.head.msgType = (kind == 3) ? MsgType::MSGTYPE_DB_TASK_REDIS : MsgType::MSGTYPE_DB_TASK_MYSQL;
    m.head.Module = Module::SYS;
    m.head.playerId = c->currentPlayerId;
    m.head.session = c->currentSession;
    m.head.epoch = c->currentEpoch;       // 结果回来时校验 Actor 生命周期
    m.head.srcWorkerId = c->workerId;     // 结果必须回到本线程，由本线程进 Lua
    m.head.ctx = (uint64_t)c->env->RegisterDbAck(cb);
    m.head.dbShard = shard;               // Redis 分片（MySQL 恒为 0）
    m.body = task.Encode();

    const uint64_t ctxId = m.head.ctx;
    if (!c->bus->sendToDb(std::move(m)))
        return PushFail(L, "db queue full");

    lua_pushinteger(L, (lua_Integer)ctxId);   // 返回 ctx，脚本可自行对账
    return 1;
}

static int l_db_query(lua_State* L) { return SendDb(L, 1); }
static int l_db_exec(lua_State* L) { return SendDb(L, 2); }
static int l_db_redis(lua_State* L) { return SendDb(L, 3); }

// ---------------- key 命名空间助手（脚本唯一合法的建 key 途径）----------------
// db.key.zone("rank")        -> "zone:1:rank"                （区服级：跨场景共享）
// db.key.scene("monster")    -> "zone:1:scene:2:monster"     （场景级）
// db.key.player(pid, "bag")  -> "zone:1:player:1001:bag"     （玩家级，不带 scene）
static int l_db_key_zone(lua_State* L)
{
    LuaApiCtx* c = Ctx(L);
    if (c == nullptr)
        return PushFail(L, "no api context");
    const char* name = luaL_optstring(L, 1, "");
    const std::string key = c->redisNs.ZoneKey(name);
    lua_pushlstring(L, key.data(), key.size());
    return 1;
}

static int l_db_key_scene(lua_State* L)
{
    LuaApiCtx* c = Ctx(L);
    if (c == nullptr)
        return PushFail(L, "no api context");
    const char* name = luaL_optstring(L, 1, "");
    const std::string key = c->redisNs.SceneKey(name);
    lua_pushlstring(L, key.data(), key.size());
    return 1;
}

static int l_db_key_player(lua_State* L)
{
    LuaApiCtx* c = Ctx(L);
    if (c == nullptr)
        return PushFail(L, "no api context");
    const uint64_t pid = (uint64_t)luaL_optinteger(L, 1, 0);
    const char* name = luaL_optstring(L, 2, "");
    if (pid == 0)
        return PushFail(L, "db.key.player(pid, name)");
    const std::string key = c->redisNs.PlayerKey(pid, name);
    lua_pushlstring(L, key.data(), key.size());
    return 1;
}

// ==================== timer ====================
static int AddTimer(lua_State* L, bool repeat)
{
    LuaApiCtx* c = Ctx(L);
    if (c == nullptr || c->env == nullptr)
        return PushFail(L, "no api context");

    const int64_t ms = (int64_t)luaL_optinteger(L, 1, 0);
    const char* fn = luaL_optstring(L, 2, nullptr);
    if (ms <= 0 || fn == nullptr)
        return PushFail(L, "timer.after(ms, fnPath)");

    const uint64_t id = c->env->AddLuaTimer(c->currentPlayerId, c->currentSession,
                                            c->currentEpoch, ms, repeat, fn);
    if (id == 0)
        return PushFail(L, "timer service unavailable");

    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int l_timer_after(lua_State* L) { return AddTimer(L, false); }
static int l_timer_every(lua_State* L) { return AddTimer(L, true); }

static int l_timer_cancel(lua_State* L)
{
    LuaApiCtx* c = Ctx(L);
    if (c == nullptr || c->env == nullptr)
        return PushFail(L, "no api context");

    const uint64_t id = (uint64_t)luaL_optinteger(L, 1, 0);
    if (id == 0)
        return PushFail(L, "timer.cancel(id)");

    c->env->CancelLuaTimer(id);
    return PushOk(L);
}

// ==================== player ====================
// 注意：这些 API 只能访问**本 worker 分片**里的玩家（同线程直调，无需加锁）。
// 返回 nil/false 表示"该玩家不在本线程"——跨分片交互必须走消息，不能跨分片直读。
static Player* LocalPlayer(lua_State* L, int idx, LuaApiCtx** outCtx)
{
    LuaApiCtx* c = Ctx(L);
    if (outCtx != nullptr)
        *outCtx = c;
    if (c == nullptr || c->players == nullptr)
        return nullptr;

    const uint64_t pid = (uint64_t)luaL_checkinteger(L, idx);
    return c->players->get(pid);
}

static int l_player_online(lua_State* L)
{
    lua_pushboolean(L, LocalPlayer(L, 1, nullptr) != nullptr ? 1 : 0);
    return 1;
}

static int l_player_session(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    lua_pushinteger(L, (lua_Integer)p->session());
    return 1;
}

static int l_player_info(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)p->id());
    lua_setfield(L, -2, "pid");
    lua_pushinteger(L, (lua_Integer)p->epoch());
    lua_setfield(L, -2, "epoch");
    lua_pushinteger(L, p->hp());
    lua_setfield(L, -2, "hp");
    lua_pushinteger(L, p->mp());
    lua_setfield(L, -2, "mp");
    lua_pushinteger(L, (lua_Integer)p->gold());
    lua_setfield(L, -2, "gold");
    lua_pushinteger(L, p->x());
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, p->y());
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, p->dir());
    lua_setfield(L, -2, "dir");
    return 1;
}

static int l_player_move(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    const int32_t x = (int32_t)luaL_checkinteger(L, 2);
    const int32_t y = (int32_t)luaL_checkinteger(L, 3);
    const int32_t dir = (int32_t)luaL_optinteger(L, 4, 0);
    p->setPos(x, y, dir);
    p->touch(NowMs());
    return PushOk(L);
}

static int l_player_modify_hp(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    p->modifyHp((int32_t)luaL_checkinteger(L, 2));
    lua_pushinteger(L, p->hp());
    return 1;
}

static int l_player_modify_mp(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    p->modifyMp((int32_t)luaL_checkinteger(L, 2));
    lua_pushinteger(L, p->mp());
    return 1;
}

static int l_player_add_gold(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    p->addGold((int64_t)luaL_checkinteger(L, 2));
    lua_pushinteger(L, (lua_Integer)p->gold());
    return 1;
}

static int l_player_bag_list(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    const std::vector<ItemStack> items = p->bag().view();
    lua_newtable(L);
    int i = 1;
    for (const auto& st : items)
    {
        lua_newtable(L);
        lua_pushinteger(L, st.itemId);
        lua_setfield(L, -2, "item_id");
        lua_pushinteger(L, st.count);
        lua_setfield(L, -2, "count");
        lua_pushinteger(L, st.slot);
        lua_setfield(L, -2, "slot");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int l_player_bag_add(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    const uint32_t itemId = (uint32_t)luaL_checkinteger(L, 2);
    const uint16_t num = (uint16_t)luaL_checkinteger(L, 3);
    if (!p->bag().addItem(itemId, num))
        return PushFail(L, "bag add failed (full or bad args)");

    p->markDirty();
    return PushOk(L);
}

static int l_player_bag_consume(lua_State* L)
{
    Player* p = LocalPlayer(L, 1, nullptr);
    if (p == nullptr)
        return PushFail(L, "player not in this worker");

    const uint32_t itemId = (uint32_t)luaL_checkinteger(L, 2);
    const uint16_t num = (uint16_t)luaL_checkinteger(L, 3);
    if (!p->bag().reduceItem(itemId, num))
        return PushFail(L, "bag consume failed (not enough)");

    p->markDirty();
    return PushOk(L);
}

// ==================== 注册到 Lua ====================
namespace
{
void RegTable(lua_State* L, const char* ns, const luaL_Reg* fns)
{
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, ns);
}

// 给已注册的模块表挂一个子表：db.key = { zone, scene, player }
void RegSubTable(lua_State* L, const char* ns, const char* sub, const luaL_Reg* fns)
{
    lua_getglobal(L, ns);
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, ns);
    }
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, sub);
    lua_pop(L, 1);
}
}  // namespace

bool RegisterAll(lua_State* L, LuaApiCtx* ctx)
{
    if (L == nullptr)
        return false;

    // 1. 上下文进 registry：绑定函数通过它拿到"本线程"的对象，替代旧的全局单例
    lua_pushlightuserdata(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, kApiCtxKey);

    // 2. 模块表
    static const luaL_Reg netLib[] = {{"send", l_net_send}, {nullptr, nullptr}};
    RegTable(L, "net", netLib);

    static const luaL_Reg dbLib[] = {{"query", l_db_query},
                                     {"exec", l_db_exec},
                                     {"redis", l_db_redis},
                                     {nullptr, nullptr}};
    RegTable(L, "db", dbLib);

    // db.key.{zone,scene,player}：脚本构造 Redis key 的**唯一合法途径**
    // （db.redis 会校验 key 前缀，裸写 key 会被拒绝）
    static const luaL_Reg dbKeyLib[] = {{"zone", l_db_key_zone},
                                        {"scene", l_db_key_scene},
                                        {"player", l_db_key_player},
                                        {nullptr, nullptr}};
    RegSubTable(L, "db", "key", dbKeyLib);

    static const luaL_Reg timerLib[] = {{"after", l_timer_after},
                                        {"every", l_timer_every},
                                        {"cancel", l_timer_cancel},
                                        {nullptr, nullptr}};
    RegTable(L, "timer", timerLib);

    static const luaL_Reg logLib[] = {{"i", l_log_info},
                                      {"w", l_log_warn},
                                      {"e", l_log_error},
                                      {nullptr, nullptr}};
    RegTable(L, "log", logLib);

    static const luaL_Reg playerLib[] = {{"online", l_player_online},
                                         {"info", l_player_info},
                                         {"session", l_player_session},
                                         {"move", l_player_move},
                                         {"modify_hp", l_player_modify_hp},
                                         {"modify_mp", l_player_modify_mp},
                                         {"add_gold", l_player_add_gold},
                                         {"bag_list", l_player_bag_list},
                                         {"bag_add", l_player_bag_add},
                                         {"bag_consume", l_player_bag_consume},
                                         {nullptr, nullptr}};
    RegTable(L, "player", playerLib);

    return true;
}

}  // namespace luaapi
