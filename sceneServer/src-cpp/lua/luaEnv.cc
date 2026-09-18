#include "luaEnv.h"
#include "TimerThread.h"
#include "log/logger.h"

// #include "game/timer_bridge.h"
#include "lua/lua_register.h"
#include "msg.h"
#include <lua.hpp>
#include <cstdint>

namespace {

// dbv::Val -> Lua 值(递归, 数组从1开始; NULL 转空串)
void PushValToLua(lua_State* L, const dbv::Val& v) {
	switch (v.kind) {
		case dbv::kInt:
			lua_pushinteger(L, (lua_Integer)v.i);
			break;
		case dbv::kStr:
			lua_pushlstring(L, v.s.data(), v.s.size());
			break;
		case dbv::kArr: 
		{
			lua_newtable(L);
			int j = 1;
			for (auto& e : v.a) {
				PushValToLua(L, e);
				lua_rawseti(L, -2, j++);
			}
			break;
		}
		default:  // NULL
			lua_pushstring(L, "");
			break;
	}
}

// 错误处理器(msgh): 触发 luaL_traceback 输出脚本行号栈回溯
static int LuaMsgh(lua_State* L) {
	const char* msg = lua_tostring(L, 1);
	if (msg == nullptr) {
		if (luaL_callmeta(L, 1, "__tostring") &&
			lua_type(L, -1) == LUA_TSTRING)
			return 1;
		msg = lua_pushfstring(L, "(error object is a %s value)",
							  luaL_typename(L, 1));
	}
	luaL_traceback(L, L, msg, 1);
	return 1;
}

}  // namespace

bool LuaEnv::Init(const std::string& root) {
	root_ = root;
	return Reload();
}

void LuaEnv::Shutdown() {
	if (L_) {
		lua_close(L_);
		L_ = nullptr;
	}
}

bool LuaEnv::Reload() {
	lua_State* fresh = luaL_newstate();
	if (!fresh) return false;
	luaL_openlibs(fresh);
	// 旧状态关闭; 若加载失败回滚
	lua_State* old = L_;
	L_ = fresh;
	if (!LoadScripts(L_)) {
		lua_close(fresh);
		L_ = old;
		return false;
	}
	if (old) lua_close(old);
	LOG_INFO<<"lua env reload ok, root= " << root_.c_str();
	return true;
}

bool LuaEnv::LoadScripts(lua_State* L) {
	// 预置错误处理器(msgh): 供 Call 使用, 失败时输出带脚本行号的栈回溯
	lua_pushcfunction(L, LuaMsgh);
	msgh_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_gc(L, LUA_GCCOLLECT, 0);

	// package.path: 支持 require
	std::string path = root_ + "/?.lua;" + root_ + "/Module/?.lua;" + root_ +
						"/core/?.lua;";
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "path");
	const char* oldpath = lua_tostring(L, -1);
	std::string np = path + (oldpath ? oldpath : "");
	lua_pop(L, 1);
	lua_pushstring(L, np.c_str());
	lua_setfield(L, -2, "path");
	lua_pop(L, 1);

	// 注册全部 C API(需在脚本执行前)
	if (!luaapi::RegisterAll(L)) {
		LOG_ERROR<<"lua register api fail";
		return false;
	}
	for (auto& f : script_files_) {
		if (!DoFile(f)) return false;
	}
	return true;
}

bool LuaEnv::DoFile(const std::string& rel_path) {
	std::string full = root_ + "/" + rel_path;
	if (luaL_loadfile(L_, full.c_str()) != 0) 
	{
		LOG_ERROR<<"lua load fail "<< full.c_str() << 
		":"<<(lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?");

		lua_pop(L_, 1);
		return false;
	}
	if (lua_pcall(L_, 0, 0, 0) != 0) 
	{
		LOG_ERROR<<"lua run fail " << full.c_str() <<
		":" <<(lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?");

		lua_pop(L_, 1);
		return false;
	}
	return true;
}

bool LuaEnv::PushFn(const char* path) {
    if (!L_) return false;
    lua_pushglobaltable(L_);
    std::string p = path;
    size_t pos = 0;
    std::string seg;
    while ((pos = p.find('.')) != std::string::npos) {
		seg = p.substr(0, pos);
		p.erase(0, pos + 1);
		lua_getfield(L_, -1, seg.c_str());
		lua_remove(L_, -2);
		int t = lua_type(L_, -1);
		if (t != LUA_TTABLE && t != LUA_TFUNCTION) {
			lua_pop(L_, 1);
			return false;
		}
    }
    lua_getfield(L_, -1, p.c_str());
    lua_remove(L_, -2);
    if (!lua_isfunction(L_, -1)) {
      LOG_ERROR<<"lua fn not found: " << path;
      lua_pop(L_, 1);
      return false;
    }
    return true;
}

bool LuaEnv::Call(int nargs) {
	++call_count_;
	int entry = lua_gettop(L_);
	int fnidx = entry - nargs;  // 函数所在栈位置
	int msgh = 0;
	if (msgh_ref_ != -2) {  // -2 == LUA_NOREF
		lua_rawgeti(L_, LUA_REGISTRYINDEX, msgh_ref_);
		lua_insert(L_, fnidx);  // msgh 置于函数之下
		msgh = fnidx;
	}
	int rc = lua_pcall(L_, nargs, 0, msgh);
	if (rc != 0) {
		++error_count_;
		const char* e = lua_tostring(L_, -1);
		LOG_ERROR<<"lua call fail: " << (e ? e : "unknown");
	}
	// 栈守卫: 无论成功/失败都恢复到函数下方(移除函数/参数/错误对象/msgh)
	lua_settop(L_, fnidx - 1);
	return rc == 0;
}

LuaEnv::DispatchResult LuaEnv::DispatchC2S(const char* path, uint32_t session,
										   uint32_t pid, int req_slot) {
	int top = lua_gettop(L_);
	if (req_slot < 0) req_slot = top + req_slot + 1;
	if (!PushFn(path)) {
		lua_settop(L_, top);
		return DispatchResult::kNoFn;
	}
	lua_pushinteger(L_, session);
	lua_pushinteger(L_, pid);
	lua_pushvalue(L_, req_slot);  // 复制请求表
	bool ok = Call(3);
	lua_settop(L_, top);  // 清理请求表, 修复栈泄漏
	return ok ? DispatchResult::kOk : DispatchResult::kLuaError;
}

int64_t LuaEnv::RegisterDbAck(const std::string& fn_path) {
	int64_t ctx = next_ctx_++;
	db_pending_[ctx] = fn_path;
	return ctx;
}

void LuaEnv::DispatchDbAck(int64_t ctx, const dbv::DbResult& res) {
	int top = lua_gettop(L_);
	auto it = db_pending_.find(ctx);
	if (it == db_pending_.end()) {
		LOG_WARNING<<"db ack ctx "<< (long long)ctx <<" not found";
		return;
	}
	std::string fn = it->second;
	db_pending_.erase(it);
	if (!PushFn(fn.c_str())) {
		lua_settop(L_, top);
		return;
	}
	lua_pushinteger(L_, (lua_Integer)ctx);
	lua_pushinteger(L_, res.errcode);
	PushValToLua(L_, res.data);
	Call(3);  // 回调签名: fn(ctx, errcode, data)
	lua_settop(L_, top);
}

uint32_t LuaEnv::AddLuaTimer(uint64_t workId, 
							 uint64_t playerId, 
							 uint64_t session, 
							 int64_t delay_ms, 
							 bool repeat, 
							 const std::string& fn) 
{
	TimerOp op;
	op.kind = TimerOp::ADD;
	op.timerId = Timer::NextTimerId();
	op.ownerWorkerId = workId;
	op.intervalMs = delay_ms;
	op.repeat = repeat;
	op.playerId = playerId;
	op.session = session;

	uint32_t id = timer_->addTimer(op);
	lua_timers_[id] = {fn, repeat};
	return id;
}

void LuaEnv::CancelLuaTimer(uint32_t timer_id) {
	timer_->cancel(timer_id);
	lua_timers_.erase(timer_id);
}

void LuaEnv::OnLuaTimerFire(uint32_t timer_id) {
	int top = lua_gettop(L_);
	auto it = lua_timers_.find(timer_id);
	if (it == lua_timers_.end()) return;
	std::string fn = it->second.first;
	bool repeat = it->second.second;
	// 一次性定时器在回调前移除注册, 避免 lua_timers_ 永久泄漏
	if (!repeat) lua_timers_.erase(it);
	if (!PushFn(fn.c_str())) {
		lua_settop(L_, top);
		return;
	}
	lua_pushinteger(L_, timer_id);
	Call(1);
	lua_settop(L_, top);
}

bool LuaEnv::CallI64(const char* path, const std::vector<int64_t>& args) {
	if (L_ == nullptr) return false;
	int top = lua_gettop(L_);
	if (!PushFn(path)) {
		lua_settop(L_, top);
		return false;
	}
	for (int64_t a : args) lua_pushinteger(L_, (lua_Integer)a);
	bool ok = Call((int)args.size());
	lua_settop(L_, top);
	return ok;
}

void LuaEnv::OnPlayerLogout(uint32_t pid) {
	if (!L_) return;
	int top = lua_gettop(L_);
	if (!PushFn("move.stop_auto_walk")) {
		lua_settop(L_, top);
		return;
	}
	lua_pushinteger(L_, (lua_Integer)pid);
	Call(1);
	lua_settop(L_, top);
}
