// ============================================================================
// lua_bind.cpp —— C++ API 绑定到 Lua 层
//   Lua 脚本通过这些 API 调用 C++ 能力(网络发包/DB/缓存/玩家状态/AOI/寻路)。
// ============================================================================
#include <lua.hpp>
#include <cstring>
#include <vector>

#include "log/logger.h"
#include "common/msg.h"
#include "db/dbValue.h"
// #include "game/combat.h"
// #include "game/game_engine.h"
// #include "game/item_conf.h"
// #include "game/player.h"
// #include "game/scene.h"
// #include "game/scene_enemy_mgr.h"
// #include "game/timer_bridge.h"
#include "lua/luaEnv.h"
#include "lua/lua_register.h"
#include "net/net_server.h"
// #include "proto/pb_lua.h"
// #include "proto/pb_registry.h"
// #include "game.pb.h"

namespace {
// ---------------- 通用取值辅助 ----------------
int64_t GetI64(lua_State* L, int idx, int64_t def) {
	int isnum = 0;
	lua_Integer v = lua_tointegerx(L, idx, &isnum);
	return isnum ? (int64_t)v : def;
}

bool ReadStrTable(lua_State* L, int idx, std::vector<std::string>& out) {
	if (lua_type(L, idx) != LUA_TTABLE) return false;
	size_t n = lua_rawlen(L, idx);
	for (size_t i = 1; i <= n; ++i) {
		lua_rawgeti(L, idx, (lua_Integer)i);
		size_t len = 0;
		const char* s = lua_tolstring(L, -1, &len);
		if (s) out.emplace_back(s, len);
		lua_pop(L, 1);
	}
	return true;
}

// SQL 模板白名单: 归一化空白后与已知模板精确匹配。
// 目的: 阻断"只要能改脚本/配置就可在逻辑线程执行任意 SQL"的越权面。
static std::string NormalizeSql(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool sp = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      sp = true;
    } else {
      if (sp && !out.empty()) out.push_back(' ');
      sp = false;
      out.push_back(c);
    }
  }
  return out;
}

static bool SqlAllowed(const std::string& sql) {
  static const char* kTemplates[] = {
      "SELECT id FROM account WHERE username=? LIMIT 1",
      "INSERT INTO account(username,password,salt) VALUES(?,?,?)",
      "INSERT INTO player(account_id,name,level,exp,hp,mp,gold,scene_id,pos_x,"
      "pos_y,dir) VALUES(?,?,1,0,100,50,1000,1,100,100,0)",
      "SELECT id,password,salt,perm FROM account WHERE username=? LIMIT 1",
      "SELECT id,name,level,exp,hp,mp,gold,scene_id,pos_x,pos_y,dir,version FROM "
      "player WHERE account_id=? LIMIT 1",
  };
  std::string n = NormalizeSql(sql);
  for (const char* t : kTemplates)
    if (n == t) return true;
  return false;
}

// 发送一条 Lua 发起的 DB/Redis 任务, 返回 ctx
int64_t SendDbFromLua(lua_State* L, uint8_t kind) {
	// 参数布局由调用方约定:
	//   kind1/2: (sql, params?, cb, [ctx])
	//   kind3  : (args[], cb, [ctx])
	std::string cb;
	std::vector<std::string> args;
	int64_t ctx = 0;
  	if (kind == 3) {
		if (!ReadStrTable(L, 1, args)) return -1;
		cb = luaL_checkstring(L, 2);
		if (lua_gettop(L) >= 3) ctx = GetI64(L, 3, 0);
  	} else {
		const char* sql = luaL_checkstring(L, 1);
		args.push_back(sql);
		std::vector<std::string> params;
		if (lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TTABLE)
			ReadStrTable(L, 2, params);
		args.insert(args.end(), params.begin(), params.end());
		cb = luaL_checkstring(L, 3);
		if (lua_gettop(L) >= 4) ctx = GetI64(L, 4, 0);
  	}
	// SQL 白名单校验(默认仅告警; 配置 sql_whitelist=1 时拒绝)
	if (kind != 3 && !args.empty() && !SqlAllowed(args[0])) {
		LOG_WARNING<<"db: sql not in whitelist:" << args[0].c_str();
		// if (AppCfg::I().sql_whitelist) return -3;
	}
	if (ctx == 0) ctx = LuaEnv::I().RegisterDbAck(cb);
    //TODO DB消息封装
	// Msg m;
	// m.h.type = MT_DB_TASK;
	// m.h.src = THREAD_LOGIC;
	// m.h.dst = THREAD_DB;
	// m.h.Module = Module::LUA_DB;          // 结果回投给 lua db 分发
	// m.h.Method = Method::LUA_DB_ACK;
	// m.h.ctx = ctx;
	// dbv::DbTask task;
	// task.kind = kind;
	// task.args = std::move(args);
	// m.body = task.Encode();
	// Bus::I().PushDbWait(std::move(m));  // DB 任务不可丢
	return ctx;
}

// JsonNode -> Lua(递归)
void PushJson(lua_State* L, const JsonNode& n) {
  switch (n.type) {
    case JsonNode::kNull:
      lua_pushnil(L);
      break;
    case JsonNode::kBool:
      lua_pushboolean(L, n.b);
      break;
    case JsonNode::kNum:
      lua_pushnumber(L, n.num);
      break;
    case JsonNode::kStr:
      lua_pushlstring(L, n.str.data(), n.str.size());
      break;
    case JsonNode::kArr: {
      lua_newtable(L);
      for (size_t i = 0; i < n.arr.size(); ++i) {
        PushJson(L, n.arr[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
      }
      break;
    }
    case JsonNode::kObj: {
      lua_newtable(L);
      for (auto& kv : n.obj) {
        PushJson(L, kv.second);
        lua_setfield(L, -2, kv.first.c_str());
      }
      break;
    }
  }
}

// ================ 业务 C 函数 ================
static int l_net_send(lua_State* L) {
	uint32_t session = (uint32_t)luaL_checkinteger(L, 1);
	const char* msg_name = luaL_checkstring(L, 2);
	if (lua_type(L, 3) != LUA_TTABLE) return luaL_error(L, "arg3 must be table");
	uint16_t seq = (uint16_t)luaL_optinteger(L, 4, 0);
	// 归属校验: session 必须存在; 若第5参数给了 pid, 还要求归属一致(防越权代发)
	if (!PlayerMgr::I().HasSession(session)) {
		LOGW("net.send: unknown session %u", session);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "session not found");
		return 2;
	}
	if (lua_gettop(L) >= 5) {
		uint32_t pid = (uint32_t)luaL_checkinteger(L, 5);
		if (pid != 0 && PlayerMgr::I().PidOfSession(session) != pid) {
			LOGW("net.send: session %u not owned by pid %u", session, pid);
			lua_pushboolean(L, 0);
			lua_pushstring(L, "session not owned by pid");
			return 2;
		}
	}
	const pbr::Route* route = pbr::FindByTypeName(msg_name);
	if (!route) return luaL_error(L, "unknown msg name: %s", msg_name);
	// 用注册表里的完整类型名("gs.RegAck")创建消息
	google::protobuf::Message* pb = pbl::NewByName(route->ack_type);
	if (!pb) return luaL_error(L, "unknown ack type: %s", route->ack_type);
	if (!pbl::TableToMessage(L, 3, pb)) {
		delete pb;
		// 字段转换失败: 回错误而不是发送"半包"
		LOGW("net.send: table->msg convert fail: %s", msg_name);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "msg convert fail");
		return 2;
	}
	std::string body = pb->SerializeAsString();
	delete pb;
	NetServer::SendPacket(session, route->Module, route->Method, seq, body);
	lua_pushboolean(L, 1);
	return 1;
}

static int l_log(lua_State* L) {
  const char* s = luaL_optstring(L, 1, "");
  LOGI("[lua] %s", s);
  return 0;
}
static int l_logw(lua_State* L) {
  const char* s = luaL_optstring(L, 1, "");
  LOGW("[lua] %s", s);
  return 0;
}
static int l_loge(lua_State* L) {
  const char* s = luaL_optstring(L, 1, "");
  LOGE("[lua] %s", s);
  return 0;
}

// db.query(sql, params?, cb) -> ctx
static int l_db_query(lua_State* L) {
	int64_t ctx = SendDbFromLua(L, 1);
	if (ctx < 0) return luaL_error(L, "db.query args error");
	lua_pushinteger(L, (lua_Integer)ctx);
	return 1;
}
static int l_db_exec(lua_State* L) {
  int64_t ctx = SendDbFromLua(L, 2);
  if (ctx < 0) return luaL_error(L, "db.exec args error");
  lua_pushinteger(L, (lua_Integer)ctx);
  return 1;
}
static int l_db_redis(lua_State* L) {
  int64_t ctx = SendDbFromLua(L, 3);
  if (ctx < 0) return luaL_error(L, "db.redis args error");
  lua_pushinteger(L, (lua_Integer)ctx);
  return 1;
}

static int l_crypto_sha256(lua_State* L) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, 1, &n);
  std::string hex = crypto::Sha256Hex(std::string(s, n));
  lua_pushlstring(L, hex.data(), hex.size());
  return 1;
}
static int l_crypto_salt(lua_State* L) {
  std::string s = crypto::RandomSaltHex();
  lua_pushlstring(L, s.data(), s.size());
  return 1;
}

static int l_timer_after(lua_State* L) {
  int64_t ms = luaL_checkinteger(L, 1);
  const char* fn = luaL_checkstring(L, 2);
  uint32_t id = LuaEnv::I().AddLuaTimer(ms, false, fn);
  lua_pushinteger(L, id);
  return 1;
}
static int l_timer_every(lua_State* L) {
  int64_t ms = luaL_checkinteger(L, 1);
  const char* fn = luaL_checkstring(L, 2);
  uint32_t id = LuaEnv::I().AddLuaTimer(ms, true, fn);
  lua_pushinteger(L, id);
  return 1;
}
static int l_timer_cancel(lua_State* L) {
  uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
  LuaEnv::I().CancelLuaTimer(id);
  return 0;
}

static Player* RequireOnline(lua_State* L, int idx) {
  uint32_t pid = (uint32_t)luaL_checkinteger(L, idx);
  Player* p = PlayerMgr::I().Online(pid);
  if (!p) {
    // 查询类 API 返回空(nil)由 Lua 判空, 不再用 luaL_error 表达业务失败
    LOGW("lua: player %u not online", pid);
    return nullptr;
  }
  return p;
}

static int l_player_online(lua_State* L) {
  lua_pushboolean(L, PlayerMgr::I().Online(
                         (uint32_t)luaL_checkinteger(L, 1)) != nullptr);
  return 1;
}
static int l_player_info(lua_State* L) {
  Player* p = RequireOnline(L, 1);
  if (!p) return 0;
  lua_newtable(L);
  lua_pushinteger(L, p->pid);
  lua_setfield(L, -2, "pid");
  lua_pushinteger(L, (lua_Integer)p->account_id);
  lua_setfield(L, -2, "account_id");
  lua_pushstring(L, p->name.c_str());
  lua_setfield(L, -2, "name");
  lua_pushinteger(L, p->level);
  lua_setfield(L, -2, "level");
  lua_pushinteger(L, (lua_Integer)p->exp);
  lua_setfield(L, -2, "exp");
  lua_pushinteger(L, p->hp);
  lua_setfield(L, -2, "hp");
  lua_pushinteger(L, p->mp);
  lua_setfield(L, -2, "mp");
  lua_pushinteger(L, (lua_Integer)p->gold);
  lua_setfield(L, -2, "gold");
  lua_pushinteger(L, p->x);
  lua_setfield(L, -2, "x");
  lua_pushinteger(L, p->y);
  lua_setfield(L, -2, "y");
  lua_pushinteger(L, p->dir);
  lua_setfield(L, -2, "dir");
  return 1;
}
static int l_player_list_bag(lua_State* L) {
  Player* p = RequireOnline(L, 1);
  if (!p) return 0;
  lua_newtable(L);
  int idx = 1;
  for (auto& it : p->bag) {
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)it.uid);
    lua_setfield(L, -2, "uid");
    lua_pushinteger(L, it.item_id);
    lua_setfield(L, -2, "item_id");
    lua_pushinteger(L, it.count);
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, it.slot);
    lua_setfield(L, -2, "slot");
    lua_rawseti(L, -2, idx++);
  }
  return 1;
}
static int l_player_Moduleify_hp(lua_State* L) {
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
  int32_t delta = (int32_t)luaL_checkinteger(L, 2);
  PlayerMgr::I().ModuleifyHp(pid, delta);
  return 0;
}
static int l_player_Moduleify_mp(lua_State* L) {
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
  int32_t delta = (int32_t)luaL_checkinteger(L, 2);
  PlayerMgr::I().ModuleifyMp(pid, delta);
  return 0;
}
static int l_player_add_gold(lua_State* L) {
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
  int64_t delta = luaL_checkinteger(L, 2);
  PlayerMgr::I().AddGold(pid, delta);
  return 0;
}
static int l_player_move(lua_State* L) {
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
  int32_t x = (int32_t)luaL_checkinteger(L, 2);
  int32_t y = (int32_t)luaL_checkinteger(L, 3);
  int32_t dir = (int32_t)luaL_optinteger(L, 4, 0);
  bool ok = PlayerMgr::I().SetPos(pid, x, y, dir);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}
static int l_player_bag_consume(lua_State* L) {
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
  uint64_t uid = (uint64_t)luaL_checkinteger(L, 2);
  int32_t num = (int32_t)luaL_checkinteger(L, 3);
  int32_t left = 0;
  int32_t item_id = 0;
  bool ok = PlayerMgr::I().BagConsume(pid, uid, num, left, item_id);
  lua_pushboolean(L, ok ? 1 : 0);
  lua_pushinteger(L, left);
  lua_pushinteger(L, item_id);  // 返回 (ok, left, item_id) 供 Lua 直接分发效果
  return 3;
}
static int l_player_bag_add(lua_State* L) {
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
  int32_t item_id = (int32_t)luaL_checkinteger(L, 2);
  int32_t count = (int32_t)luaL_optinteger(L, 3, 1);
  PlayerMgr::I().BagAdd(pid, item_id, count);
  return 0;
}
static int l_player_kick(lua_State* L) {
    uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
    PlayerMgr::I().Logout(pid, true);
    return 0;
}
// 查询玩家当前会话(供附近广播等安全发送使用)
static int l_player_session(lua_State* L) {
    uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, PlayerMgr::I().SessionOf(pid));
    return 1;
}
static int l_player_get_nearby(lua_State* L) {
    Player* p = RequireOnline(L, 1);
    if (!p) return 0;
    std::vector<uint32_t> near = Scene::I().GetNearby(p->pid);
    lua_newtable(L);
    int idx = 1;
    for (uint32_t np : near) {
        Player* o = PlayerMgr::I().Online(np);
        if (!o) continue;
        lua_newtable(L);
        lua_pushinteger(L, o->pid);
        lua_setfield(L, -2, "pid");
        lua_pushstring(L, o->name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, o->x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, o->y);
        lua_setfield(L, -2, "y");
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

static int l_item_conf(lua_State* L) {
    int32_t item_id = (int32_t)luaL_checkinteger(L, 1);
    const ItemConf* conf = ItemConfMgr::I().Find(item_id);
    if (!conf) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, conf->item_id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, conf->name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, conf->item_type);
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, conf->max_stack);
    lua_setfield(L, -2, "stack");
    lua_pushstring(L, conf->use_func.c_str());
    lua_setfield(L, -2, "func");
    // 参数 JSON 树 -> Lua
    if (conf->param.type == JsonNode::kObj) {
        PushJson(L, conf->param);
        lua_setfield(L, -2, "param");
    }
    return 1;
}

static int l_path_find(lua_State* L) {
    int sx = (int)luaL_checkinteger(L, 1);
    int sy = (int)luaL_checkinteger(L, 2);
    int tx = (int)luaL_checkinteger(L, 3);
    int ty = (int)luaL_checkinteger(L, 4);
    auto path = Scene::I().FindPath(sx, sy, tx, ty);
    lua_newtable(L);
    int idx = 1;
    for (auto& p : path) {
        lua_newtable(L);
        lua_pushinteger(L, p.first);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, p.second);
        lua_setfield(L, -2, "y");
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

static int64_t FieldI64(lua_State* L, int tbl, const char* key, int64_t def) 
{
    lua_getfield(L, tbl, key);
    int64_t v = GetI64(L, -1, def);
    lua_pop(L, 1);
    return v;
}

static int l_player_login_ok(lua_State* L) 
{
    uint32_t session = (uint32_t)luaL_checkinteger(L, 1);
    int tbl = 2;
    if (lua_type(L, tbl) != LUA_TTABLE) return luaL_error(L, "arg2 not table");
    PlayerLoginData d;
    d.account_id = (uint64_t)FieldI64(L, tbl, "account_id", 0);
    d.pid = (uint32_t)FieldI64(L, tbl, "pid", 0);
    lua_getfield(L, tbl, "name");
    const char* name = lua_tostring(L, -1);
    if (name) d.name = name;
    lua_pop(L, 1);
    d.level = (int32_t)FieldI64(L, tbl, "level", 1);
    d.exp = FieldI64(L, tbl, "exp", 0);
    d.hp = (int32_t)FieldI64(L, tbl, "hp", 100);
    d.mp = (int32_t)FieldI64(L, tbl, "mp", 50);
    d.gold = FieldI64(L, tbl, "gold", 0);
    d.scene = (int32_t)FieldI64(L, tbl, "scene", 1);
    d.x = (int32_t)FieldI64(L, tbl, "x", 100);
    d.y = (int32_t)FieldI64(L, tbl, "y", 100);
    d.dir = (int32_t)FieldI64(L, tbl, "dir", 0);
    d.perm = (uint32_t)FieldI64(L, tbl, "perm", 0);
    d.version = (uint32_t)FieldI64(L, tbl, "version", 0);
    bool ok = PlayerMgr::I().EnterWorld(session, d);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// 逻辑线程主循环空闲时执行热更(避免在 Lua 调用栈内重载)
static int l_admin_reload(lua_State* L) {
  RequestLuaReload();
  lua_pushboolean(L, 1);
  return 1;
}

// ================ 敌人(供 enemy_ai.lua 决策 + GM/调试) ================
static Enemy* EnemyOrNull(lua_State* L, int idx) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, idx);
  return SceneEnemyMgr::I().Find(eid);
}
static int l_enemy_info(lua_State* L) {
  Enemy* e = EnemyOrNull(L, 1);
  if (!e) {
    lua_pushnil(L);
    return 1;
  }
  const int64_t seen =
      e->TargetPid() ? NowMs() - e->TargetSeenSinceMs() : 0;
  lua_newtable(L);
  lua_pushinteger(L, e->Eid());
  lua_setfield(L, -2, "eid");
  lua_pushinteger(L, e->EnemyId());
  lua_setfield(L, -2, "enemy_id");
  lua_pushinteger(L, (lua_Integer)e->St());
  lua_setfield(L, -2, "state");
  lua_pushinteger(L, e->Hp());
  lua_setfield(L, -2, "hp");
  lua_pushinteger(L, e->MaxHp());
  lua_setfield(L, -2, "max_hp");
  lua_pushinteger(L, e->X());
  lua_setfield(L, -2, "x");
  lua_pushinteger(L, e->Y());
  lua_setfield(L, -2, "y");
  lua_pushinteger(L, e->TargetPid());
  lua_setfield(L, -2, "target_pid");
  lua_pushinteger(L, seen);
  lua_setfield(L, -2, "seen_ms");
  lua_pushinteger(L, e->ChaseDwellMs());
  lua_setfield(L, -2, "chase_dwell_ms");
  lua_pushinteger(L, e->ChaseRange());
  lua_setfield(L, -2, "chase_range");
  lua_pushinteger(L, e->LeashRange());
  lua_setfield(L, -2, "leash_range");
  lua_pushinteger(L, e->AttackRange());
  lua_setfield(L, -2, "attack_range");
  lua_pushinteger(L, e->PatrolRadius());
  lua_setfield(L, -2, "patrol_radius");
  lua_pushboolean(L, e->Alive() ? 1 : 0);
  lua_setfield(L, -2, "alive");
  return 1;
}
static int l_enemy_set_state(lua_State* L) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 1);
  const char* name = luaL_checkstring(L, 2);
  State st;
  if (strcmp(name, "patrol") == 0)
    st = kPatrolling;
  else if (strcmp(name, "chase") == 0)
    st = kChase;
  else if (strcmp(name, "attack") == 0)
    st = kAttack;
  else if (strcmp(name, "injured") == 0)
    st = kInjured;
  else if (strcmp(name, "death") == 0)
    st = kDeath;
  else
    return luaL_error(L, "bad enemy state: %s", name);
  lua_pushboolean(L, SceneEnemyMgr::I().SetState(eid, st) ? 1 : 0);
  return 1;
}
static int l_enemy_mark_target(lua_State* L) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 1);
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 2);
  lua_pushboolean(L, SceneEnemyMgr::I().MarkTarget(eid, pid) ? 1 : 0);
  return 1;
}
static int l_enemy_clear_target(lua_State* L) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 1);
  lua_pushboolean(L, SceneEnemyMgr::I().ClearTarget(eid) ? 1 : 0);
  return 1;
}
static int l_enemy_nearby_players(lua_State* L) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 1);
  int radius = (int)luaL_optinteger(L, 2, 0);
  std::vector<uint32_t> list = SceneEnemyMgr::I().NearbyPlayers(eid, radius);
  lua_newtable(L);
  int idx = 1;
  for (uint32_t pid : list) {
    Player* p = PlayerMgr::I().Online(pid);
    if (!p) continue;
    lua_newtable(L);
    lua_pushinteger(L, p->pid);
    lua_setfield(L, -2, "pid");
    lua_pushinteger(L, p->x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, p->y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, p->hp);
    lua_setfield(L, -2, "hp");
    lua_rawseti(L, -2, idx++);
  }
  return 1;
}
static int l_enemy_step_toward(lua_State* L) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 1);
  int tx = (int)luaL_checkinteger(L, 2);
  int ty = (int)luaL_checkinteger(L, 3);
  lua_pushboolean(L, SceneEnemyMgr::I().StepToward(eid, tx, ty) ? 1 : 0);
  return 1;
}
static int l_enemy_patrol_step(lua_State* L) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 1);
  lua_pushboolean(L, SceneEnemyMgr::I().PatrolStep(eid) ? 1 : 0);
  return 1;
}
static int l_enemy_attack_player(lua_State* L) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 1);
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 2);
  int dmg = 0, hp = 0;
  bool dead = false;
  bool ok = SceneEnemyMgr::I().AttackPlayer(eid, pid, &dmg, &hp, &dead);
  lua_pushboolean(L, ok ? 1 : 0);
  lua_pushinteger(L, dmg);
  lua_pushinteger(L, hp);
  lua_pushboolean(L, dead ? 1 : 0);
  return 4;
}
static int l_enemy_damage(lua_State* L) {
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 1);
  int dmg = (int)luaL_checkinteger(L, 2);
  uint32_t src = (uint32_t)luaL_optinteger(L, 3, 0);
  int hp = 0;
  bool dead = false;
  bool ok = SceneEnemyMgr::I().Damage(eid, dmg, src, &hp, &dead);
  lua_pushboolean(L, ok ? 1 : 0);
  lua_pushinteger(L, hp);
  lua_pushboolean(L, dead ? 1 : 0);
  return 3;
}
static int l_enemy_list(lua_State* L) {
  int32_t scene_id = (int32_t)luaL_optinteger(L, 1, 1);
  std::vector<uint32_t> ids = SceneEnemyMgr::I().ListByScene(scene_id);
  lua_newtable(L);
  int idx = 1;
  for (uint32_t eid : ids) {
    lua_pushinteger(L, eid);
    lua_rawseti(L, -2, idx++);
  }
  return 1;
}
static int l_enemy_scene_restart(lua_State* L) {
  int32_t scene_id = (int32_t)luaL_optinteger(L, 1, 1);
  SceneEnemyMgr::I().OnSceneRestart(scene_id);  // 场景重启 -> 敌人重新刷新
  lua_pushboolean(L, 1);
  return 1;
}

// ================ 玩家攻击 / 仓库 ================
static int l_combat_player_attack(lua_State* L) {
  uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
  uint32_t eid = (uint32_t)luaL_checkinteger(L, 2);
  AttackResult r;
  // 返回 (ret, dmg, target_hp, dead): ret==0 表示服务端校验通过并已结算
  int ret = Combat::I().PlayerAttackEnemy(pid, eid, r);
  lua_pushinteger(L, ret);
  lua_pushinteger(L, r.dmg);
  lua_pushinteger(L, r.target_hp);
  lua_pushboolean(L, r.dead ? 1 : 0);
  return 4;
}
static int l_player_list_warehouse(lua_State* L) {
  Player* p = RequireOnline(L, 1);
  if (!p) return 0;
  lua_newtable(L);
  int idx = 1;
  for (auto& it : p->warehouse) {
    lua_newtable(L);
    lua_pushinteger(L, it.uid);
    lua_setfield(L, -2, "uid");
    lua_pushinteger(L, it.item_id);
    lua_setfield(L, -2, "item_id");
    lua_pushinteger(L, it.count);
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, it.slot);
    lua_setfield(L, -2, "slot");
    lua_rawseti(L, -2, idx++);
  }
  return 1;
}
static int l_player_give_item(lua_State* L) 
{
    uint32_t pid = (uint32_t)luaL_checkinteger(L, 1);
    int32_t item_id = (int32_t)luaL_checkinteger(L, 2);
    int32_t count = (int32_t)luaL_optinteger(L, 3, 1);
    // 返回 (placed, overflow, placed_wh, used_warehouse)
    BagPutResult r = PlayerMgr::I().GiveItem(pid, item_id, count);
    lua_pushinteger(L, r.placed);
    lua_pushinteger(L, r.overflow);
    lua_pushinteger(L, r.placed_wh);
    lua_pushboolean(L, r.used_warehouse ? 1 : 0);
    return 4;
}

// ================ 注册到 Lua ================
static void Reg(lua_State* L, const char* ns, const luaL_Reg* fns) 
{
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, ns);
}
static void RegFn(lua_State* L, 
                  const char* ns, 
                  const char* name,
                  lua_CFunction fn) 
{
    lua_getglobal(L, ns);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
    lua_setglobal(L, ns);
}

}  // namespace

namespace luaapi {
/**
 * @brief 把 C++ 函数注册成 Lua 可调用的全局 API
 * 
 */
bool RegisterAll(lua_State* L) {
	static const luaL_Reg net_lib[] = {{"send", l_net_send}, {nullptr, nullptr}};
	Reg(L, "net", net_lib);
	RegFn(L, "log", "i", l_log);
	RegFn(L, "log", "w", l_logw);
	RegFn(L, "log", "e", l_loge);

	static const luaL_Reg db_lib[] = {
		{"query", l_db_query}, 
		{"exec", l_db_exec}, 
		{"redis", l_db_redis},
		{nullptr, nullptr}};
	Reg(L, "db", db_lib);

	static const luaL_Reg crypto_lib[] = {{"sha256", l_crypto_sha256},
											{"salt", l_crypto_salt},
											{nullptr, nullptr}};
	Reg(L, "crypto", crypto_lib);

	static const luaL_Reg timer_lib[] = {{"after", l_timer_after},
										{"every", l_timer_every},
										{"cancel", l_timer_cancel},
										{nullptr, nullptr}};
	Reg(L, "timer", timer_lib);

	RegFn(L, "player", "online", l_player_online);
	RegFn(L, "player", "info", l_player_info);
	RegFn(L, "player", "list_bag", l_player_list_bag);
	RegFn(L, "player", "Moduleify_hp", l_player_Moduleify_hp);
	RegFn(L, "player", "Moduleify_mp", l_player_Moduleify_mp);
	RegFn(L, "player", "add_gold", l_player_add_gold);
	RegFn(L, "player", "move", l_player_move);
	RegFn(L, "player", "bag_consume", l_player_bag_consume);
	RegFn(L, "player", "bag_add", l_player_bag_add);
	RegFn(L, "player", "kick", l_player_kick);
	RegFn(L, "player", "session", l_player_session);
	RegFn(L, "player", "nearby", l_player_get_nearby);
	RegFn(L, "player", "login_ok", l_player_login_ok);
	RegFn(L, "player", "list_warehouse", l_player_list_warehouse);
	RegFn(L, "player", "give_item", l_player_give_item);

	// ---- 敌人(Enemy) 与 战斗(Combat) ----
	RegFn(L, "enemy", "info", l_enemy_info);
	RegFn(L, "enemy", "list", l_enemy_list);
	RegFn(L, "enemy", "set_state", l_enemy_set_state);
	RegFn(L, "enemy", "mark_target", l_enemy_mark_target);
	RegFn(L, "enemy", "clear_target", l_enemy_clear_target);
	RegFn(L, "enemy", "nearby_players", l_enemy_nearby_players);
	RegFn(L, "enemy", "step_toward", l_enemy_step_toward);
	RegFn(L, "enemy", "patrol_step", l_enemy_patrol_step);
	RegFn(L, "enemy", "attack_player", l_enemy_attack_player);
	RegFn(L, "enemy", "damage", l_enemy_damage);
	RegFn(L, "enemy", "scene_restart", l_enemy_scene_restart);
	RegFn(L, "combat", "player_attack_enemy", l_combat_player_attack);

	RegFn(L, "item", "conf", l_item_conf);
	RegFn(L, "path", "find", l_path_find);
	RegFn(L, "admin", "reload", l_admin_reload);
	return true;
}

}  // namespace luaapi
