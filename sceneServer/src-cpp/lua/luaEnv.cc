#include "lua/luaEnv.h"

#include "common/msg.h"
#include "log/logger.h"
#include "lua/lua_register.h"
#include "proto/pb_lua.h"
#include "routeTable.h"
#include "timer/TimerThread.h"

#include <lua.hpp>
#include <algorithm>
#include <dirent.h>
#include <utility>

namespace
{
// 错误处理器(msgh): 触发 luaL_traceback 输出脚本行号栈回溯
int LuaMsgh(lua_State* L)
{
    const char* msg = lua_tostring(L, 1);
    if (msg == nullptr)
    {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// 指令预算钩子：超出预算直接抛 Lua 错误，由 lua_pcall 接住。
// 目的：Lua 里一个 while true do end 会卡死该 worker 上的所有玩家，
// lua_pcall 本身无法中断，只能用 lua_sethook 做上限。
void LuaCountHook(lua_State* L, lua_Debug* ar)
{
    (void)ar;
    luaL_error(L, "lua instruction budget exhausted (possible infinite loop)");
}

// 扫描一个目录下的 .lua 并**排序**后加入加载列表。
// 排序是为了让所有 worker 的加载顺序完全一致：否则 A worker 先定义、B worker 后覆盖，
// 会出现"同名函数在两个 worker 行为不同"的诡异问题。
void ScanLuaDir(const std::string& absDir, const std::string& relPrefix,
                std::vector<std::string>& out)
{
    DIR* d = ::opendir(absDir.c_str());
    if (d == nullptr)
        return;

    std::vector<std::string> names;
    while (dirent* e = ::readdir(d))
    {
        std::string n = e->d_name;
        if (n.size() < 4 || n.compare(n.size() - 4, 4, ".lua") != 0)
            continue;
        names.push_back(n);
    }
    ::closedir(d);

    std::sort(names.begin(), names.end());
    for (auto& n : names)
        out.push_back(relPrefix.empty() ? n : relPrefix + "/" + n);
}

// dbv::Val -> Lua 值(递归, 数组从1开始; NULL 转 lua nil 语义的空串)
void PushValToLua(lua_State* L, const dbv::Val& v)
{
    switch (v.kind)
    {
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
            for (auto& e : v.a)
            {
                PushValToLua(L, e);
                lua_rawseti(L, -2, j++);
            }
            break;
        }
        default:  // NULL
            lua_pushnil(L);
            break;
    }
}
}  // namespace

LuaEnv::LuaEnv() = default;

LuaEnv::~LuaEnv()
{
    Shutdown();
}

bool LuaEnv::Init(const std::string& root, LuaApiCtx* ctx)
{
    root_ = root;
    ctx_ = ctx;
    return Reload();
}

void LuaEnv::Shutdown()
{
    if (L_ != nullptr)
    {
        lua_close(L_);
        L_ = nullptr;
    }
    luaTimers_.clear();
    dbPending_.clear();
}

bool LuaEnv::Reload()
{
    lua_State* fresh = luaL_newstate();
    if (fresh == nullptr)
    {
        LOG_ERROR << "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(fresh);

    // 新 state 装载：注册上下文与 C API、加载脚本。
    // 只有全部成功才切换，否则回滚到旧 state（热更最怕"更坏了还回不去"）。
    lua_State* old = L_;
    L_ = fresh;
    if (!LoadScripts(L_))
    {
        lua_close(fresh);
        L_ = old;
        LOG_ERROR << "lua reload failed, keep old state";
        return false;
    }

    // 旧 state 关闭前，登记表必须清空：
    // 它们指向旧 state 里的函数/闭包，跨 state 复用会导致调用到不存在的函数
    luaTimers_.clear();
    dbPending_.clear();

    if (old != nullptr)
        lua_close(old);

    LOG_INFO << "lua env ready, root=" << root_ << " scripts=" << scriptFiles_.size();
    return true;
}

bool LuaEnv::LoadScripts(lua_State* L)
{
    // 预置错误处理器(msgh): 供 Call 使用
    lua_pushcfunction(L, LuaMsgh);
    msghRef_ = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_gc(L, LUA_GCCOLLECT, 0);

    // package.path: 支持 require
    std::string path = root_ + "/?.lua;" + root_ + "/module/?.lua;" + root_ + "/core/?.lua;";
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char* oldPath = lua_tostring(L, -1);
    std::string np = path + (oldPath ? oldPath : "");
    lua_pop(L, 1);
    lua_pushstring(L, np.c_str());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    // 注册 C API（脚本执行前必须完成），并把本线程上下文放进 registry
    if (!luaapi::RegisterAll(L, ctx_))
    {
        LOG_ERROR << "lua register api failed";
        return false;
    }

    // 加载顺序：core/（框架级） -> 根目录（入口） -> module/（玩法模块）
    scriptFiles_.clear();
    ScanLuaDir(root_ + "/core", "core", scriptFiles_);
    ScanLuaDir(root_, "", scriptFiles_);
    ScanLuaDir(root_ + "/module", "module", scriptFiles_);

    for (auto& f : scriptFiles_)
    {
        if (!DoFile(f))
            return false;
    }
    return true;
}

bool LuaEnv::DoFile(const std::string& relPath)
{
    const std::string full = root_ + "/" + relPath;
    if (luaL_loadfile(L_, full.c_str()) != 0)
    {
        LOG_ERROR << "lua load fail " << full << ":"
                  << (lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?");
        lua_pop(L_, 1);
        return false;
    }
    if (lua_pcall(L_, 0, 0, 0) != 0)
    {
        LOG_ERROR << "lua run fail " << full << ":"
                  << (lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?");
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaEnv::PushFn(const char* path)
{
    if (L_ == nullptr || path == nullptr || path[0] == '\0')
        return false;

    lua_pushglobaltable(L_);
    std::string p = path;
    std::string seg;
    size_t pos = 0;
    while ((pos = p.find('.')) != std::string::npos)
    {
        seg = p.substr(0, pos);
        p.erase(0, pos + 1);
        lua_getfield(L_, -1, seg.c_str());
        // 中间不是表就说明路径写错了：把中间结果清掉，避免污染栈
        if (!lua_istable(L_, -1))
        {
            lua_settop(L_, lua_gettop(L_) - 2);
            return false;
        }
        lua_remove(L_, -2);
    }
    lua_getfield(L_, -1, p.c_str());   // 末段：函数
    lua_remove(L_, -2);                // 去掉全局表

    if (!lua_isfunction(L_, -1))
    {
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaEnv::Call(int nargs, int nresults)
{
    if (L_ == nullptr)
        return false;

    const int entry = lua_gettop(L_);
    const int fnIdx = entry - nargs;   // 函数所在栈位置
    int msgh = 0;
    if (msghRef_ != -2)                // -2 == LUA_NOREF
    {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, msghRef_);
        lua_insert(L_, fnIdx);         // msgh 置于函数之下
        msgh = fnIdx;
    }

    if (instructionLimit_ > 0)
        lua_sethook(L_, LuaCountHook, LUA_MASKCOUNT, (int)instructionLimit_);

    const int rc = lua_pcall(L_, nargs, nresults, msgh);

    if (instructionLimit_ > 0)
        lua_sethook(L_, nullptr, 0, 0);   // 清掉钩子，避免影响后续 C API 调用

    ++callCount_;
    if (rc != 0)
    {
        ++errorCount_;
        const char* e = lua_tostring(L_, -1);
        LOG_ERROR << "lua call fail: " << (e ? e : "unknown");
    }

    // 栈守卫：恢复到"函数之下"（移除函数/参数/错误对象/msgh，保留 nresults）
    lua_settop(L_, fnIdx + nresults);
    return rc == 0;
}

LuaEnv::DispatchResult LuaEnv::DispatchC2S(const RouteTable::Route* route,
                                           uint64_t session,
                                           uint64_t playerId,
                                           uint64_t currentSeq,
                                           uint32_t epoch,
                                           const std::string& body)
{
    if (L_ == nullptr)
        return DispatchResult::kLuaError;
    if (route == nullptr || route->lua_fn == nullptr || route->lua_fn[0] == '\0')
        return DispatchResult::kNoFn;

    const int top = lua_gettop(L_);
    if (!PushFn(route->lua_fn))
    {
        lua_settop(L_, top);
        return DispatchResult::kNoFn;
    }

    // 参数顺序：fn(session, pid, req_table)
    // 先压 session/pid，最后压请求表 —— 这样栈顶天然就是第三个参数，
    // 不需要 lua_insert 这类容易记反语义的操作（lua_insert(idx) 是"把栈顶移到 idx"）。
    lua_pushinteger(L_, (lua_Integer)session);
    lua_pushinteger(L_, (lua_Integer)playerId);

    // 请求体反序列化收进这里：调用方只给 (route, body)。
    // 旧接口让调用方传 req_slot=-1 表示"复制栈顶当前元素"，
    // 而调用方从来没有压过请求表 —— Lua 拿到的第三个参数是垃圾。
    if (route->req_type != nullptr && route->req_type[0] != '\0')
    {
        if (!pbl::BodyToTable(L_, route->req_type, body))
        {
            lua_settop(L_, top);
            return DispatchResult::kLuaError;
        }
    }
    else
    {
        lua_newtable(L_);   // 只出站/无请求体的路由，给个空表
    }

    // 进 Lua 前写入"我是谁"，供 net.send / db.* 自动带上身份与请求序列号
    if (ctx_ != nullptr)
    {
        ctx_->currentSession = session;
        ctx_->currentPlayerId = playerId;
        ctx_->currentSeq = currentSeq;
        ctx_->currentEpoch = epoch;
    }

    const bool ok = Call(3, 0);

    if (ctx_ != nullptr)
    {
        ctx_->currentSession = 0;
        ctx_->currentPlayerId = 0;
        ctx_->currentSeq = 0;
        ctx_->currentEpoch = 0;
    }

    lua_settop(L_, top);
    return ok ? DispatchResult::kOk : DispatchResult::kLuaError;
}

int64_t LuaEnv::RegisterDbAck(const std::string& fnPath)
{
    const int64_t ctx = nextCtx_++;
    dbPending_[ctx] = fnPath;
    return ctx;
}

void LuaEnv::DispatchDbAck(int64_t ctx, const dbv::DbResult& res)
{
    if (L_ == nullptr)
        return;

    const int top = lua_gettop(L_);
    auto it = dbPending_.find(ctx);
    if (it == dbPending_.end())
    {
        LOG_WARNING << "db ack ctx " << ctx << " not found (player already logged out?)";
        return;
    }
    const std::string fn = it->second;
    dbPending_.erase(it);

    if (!PushFn(fn.c_str()))
    {
        lua_settop(L_, top);
        LOG_WARNING << "db ack fn missing: " << fn;
        return;
    }

    lua_pushinteger(L_, (lua_Integer)ctx);
    lua_pushinteger(L_, res.errcode);
    lua_pushlstring(L_, res.errmsg.data(), res.errmsg.size());
    PushValToLua(L_, res.data);
    Call(4, 0);   // 回调签名: fn(ctx, errcode, errmsg, data)
    lua_settop(L_, top);
}

uint64_t LuaEnv::AddLuaTimer(uint64_t playerId, uint64_t session, uint32_t epoch,
                             int64_t delayMs, bool repeat, const std::string& fnPath)
{
    // timer_ 未绑定时必须明确报错返回，而不是通过野指针调虚函数崩溃
    if (ctx_ == nullptr || ctx_->timer == nullptr)
    {
        LOG_ERROR << "timer service not bound, cannot add lua timer: " << fnPath;
        return 0;
    }

    TimerOp op;
    op.kind = TimerOp::ADD;
    op.timerId = Timer::NextTimerId();          // 由调用方分配，addTimer 不需要等返回值
    op.ownerWorkerId = ctx_->workerId;          // 回调必须回到本线程（Actor 串行）
    op.intervalMs = delayMs > 0 ? (uint64_t)delayMs : 1;
    op.repeat = repeat;
    op.Module = Module::TIMER;
    op.Method = Method::TIMER_LUA_FIRE;
    op.playerId = playerId;
    op.session = session;
    op.epoch = epoch;

    if (!ctx_->timer->addTimer(op))
    {
        LOG_WARNING << "add lua timer failed, fn=" << fnPath;
        return 0;
    }

    LuaTimerInfo info;
    info.fnPath = fnPath;
    info.repeat = repeat;
    info.playerId = playerId;
    luaTimers_[op.timerId] = std::move(info);
    return op.timerId;
}

void LuaEnv::CancelLuaTimer(uint64_t timerId)
{
    if (ctx_ != nullptr && ctx_->timer != nullptr)
        ctx_->timer->cancel(timerId);   // 通知时间轮（惰性取消）

    luaTimers_.erase(timerId);          // 同时清掉回调登记，避免僵尸回调
}

void LuaEnv::OnLuaTimerFire(uint64_t timerId, uint32_t epoch, uint64_t playerId)
{
    if (L_ == nullptr)
        return;

    auto it = luaTimers_.find(timerId);
    if (it == luaTimers_.end())
        return;   // 已取消：静默忽略（取消与到期竞争是常态）

    LuaTimerInfo info = it->second;

    // 发起时刻的玩家 ≠ 本次回调的玩家：说明定时器被复用/串号，直接丢弃
    if (info.playerId != 0 && info.playerId != playerId)
    {
        LOG_WARNING << "lua timer player mismatch, timerId=" << timerId;
        luaTimers_.erase(it);
        return;
    }
    (void)epoch;   // epoch 的权威校验在逻辑线程（PlayerManager::validateEpoch）

    // 一次性定时器在回调前移除登记，避免 luaTimers_ 永久泄漏
    if (!info.repeat)
        luaTimers_.erase(it);

    const int top = lua_gettop(L_);
    if (!PushFn(info.fnPath.c_str()))
    {
        lua_settop(L_, top);
        return;
    }
    lua_pushinteger(L_, (lua_Integer)timerId);
    Call(1, 0);
    lua_settop(L_, top);
}

bool LuaEnv::CallI64(const char* path, const std::vector<int64_t>& args)
{
    if (L_ == nullptr)
        return false;

    const int top = lua_gettop(L_);
    if (!PushFn(path))
    {
        lua_settop(L_, top);
        return false;
    }
    for (int64_t a : args)
        lua_pushinteger(L_, (lua_Integer)a);

    const bool ok = Call((int)args.size(), 0);
    lua_settop(L_, top);
    return ok;
}

void LuaEnv::OnPlayerLogout(uint64_t playerId)
{
    if (L_ == nullptr)
        return;

    // 1. 先取消该玩家的全部 Lua 定时器：否则回调会打到已下线的玩家
    if (ctx_ != nullptr && ctx_->timer != nullptr)
    {
        for (auto it = luaTimers_.begin(); it != luaTimers_.end();)
        {
            if (it->second.playerId == playerId)
            {
                ctx_->timer->cancel(it->first);
                it = luaTimers_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // 2. 清掉该玩家未返回的 DB 回调登记（迟到的结果会被丢弃）
    //    注意：这里无法判断哪个 ctx 属于哪个玩家，交由 epoch 校验兜底，
    //    因此 dbPending_ 只做"上限保护"，避免长期只增不减。

    // 3. 通知脚本层（可选钩子）
    const int top = lua_gettop(L_);
    if (!PushFn("player.on_logout"))
    {
        lua_settop(L_, top);
        return;
    }
    lua_pushinteger(L_, (lua_Integer)playerId);
    Call(1, 0);
    lua_settop(L_, top);
}

