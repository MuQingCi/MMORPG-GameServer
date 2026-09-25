#ifndef CLEARMOON_SCENE_LUA_LUAENV_H
#define CLEARMOON_SCENE_LUA_LUAENV_H

#include "common/ITimerService.h"
#include "db/dbValue.h"
#include "lua/luaApiCtx.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct lua_State;
class MsgBus;
class PlayerManager;

namespace RouteTable
{
struct Route;
}

/**
 * @brief 逻辑线程独占的 Lua 环境 —— 一个实例持有且只持有一个lua_State
 *
 * 为什么必须 per-thread（设计文档 3.2.3 修正）：
 *   Lua 的线程安全模型是硬约束 —— 一个 lua_State 同一时刻只能被一个线程使用，
 *   其 _G/registry 都是 state 私有的。全局单例 LuaEnv + N 个逻辑线程并发调用
 *   会破坏 Lua 栈与 GC（随机崩溃/错乱），加锁也没用（C API 调用期间不能放锁，
 *   一放锁 N 线程直接串行化，多逻辑线程失去意义）。
 *
 * 本类的职责边界：
 *   - 只做"把 C++ 的调用翻译成 Lua 调用"，不持有任何业务状态；
 *   - 所有进入 Lua 的路径都用 lua_pcall + 栈平衡守卫（gettop/settop），任何 Lua 侧报错都被接住并转成日志 + 错误码，绝不让异常穿透到 C++；
 *   - 不对外暴露 lua_State*（用类型设计禁止"从别处拿 state 乱用"）。
 *
 * 生命周期：Reload() 会 luaL_newstate() 重建 state。
 *   这意味着——所有 Lua 侧闭包/协程/定时器注册都会消失。
   因此：
 *   1) 热更走"目录即版本"（/lua/scripts/20250114_1530/），worker 不会读到被覆盖的文件；
 *   2) Lua 侧不得保存"热更后无法从 C++ 重建的状态"；
 *   3) 定时器/DB 回调的登记表由本类（C++ 侧）持有，跨热更仍然有效。
 */
class LuaEnv
{
public:
    // C2S 分发结果: 供逻辑层决定是否回 RetTip
    enum class DispatchResult { kOk, kNoFn, kLuaError };

    LuaEnv();
    ~LuaEnv();

    LuaEnv(const LuaEnv&) = delete;
    LuaEnv& operator=(const LuaEnv&) = delete;

    // 初始化：创建 state、注册绑定表、加载脚本
    bool Init(const std::string& root, LuaApiCtx* ctx);
    void Shutdown();
    bool Reload();   // 热更: 重建 state 并重新加载全部脚本(失败自动回滚到旧 state)

    bool ready() const { return L_ != nullptr; }

    // 把 "a.b.c" 路径的函数压到栈顶; 返回是否可调用
    bool PushFn(const char* path);
    // 调用栈顶函数, nargs 为已压参数个数
    // nresults: 需要保留的返回值个数(默认 0)；AI/工具类调用会用到 > 0
    // 返回 false 表示函数缺失或执行出错（调用方需兜底）
    bool Call(int nargs, int nresults = 0);

    // ---- C2S 业务入口 ----
    // route 给出 req_type/lua_fn；body 是本线程反序列化后的 protobuf 字节流。
    // 调用 Lua 处理函数 fn(session, pid, req_table)；seq 会记录进上下文，
    // 由 net.send 默认"原样回传"（脚本不必自己转发 seq）。
    DispatchResult DispatchC2S(const RouteTable::Route* route,
                               uint64_t session,
                               uint64_t playerId,
                               uint64_t seq,
                               uint32_t epoch,
                               const std::string& body);

    // ---- DB 回调 ----
    // 登记一个 Lua 回调函数，返回 ctx（随后随 DB 任务下发）
    int64_t RegisterDbAck(const std::string& fnPath);
    // 由 owner 逻辑线程调用: fn(ctx, errcode, errmsg, data)
    void DispatchDbAck(int64_t ctx, const dbv::DbResult& res);

    // ---- Lua 定时器 ----
    // 返回 timerId(0 表示失败)。ownerWorkerId 由 ctx 提供，保证回调回到本线程
    uint64_t AddLuaTimer(uint64_t playerId, uint64_t session, uint32_t epoch,
                         int64_t delayMs, bool repeat, const std::string& fnPath);
    void CancelLuaTimer(uint64_t timerId);
    // 定时器到期：先校验 epoch（Actor 可能已下线重建），再调用 Lua 回调
    void OnLuaTimerFire(uint64_t timerId, uint32_t epoch, uint64_t playerId);

    // 以整数参数调用 "a.b.c"（C++ 驱动 Lua 决策，例如敌人 AI tick）
    bool CallI64(const char* path, const std::vector<int64_t>& args);

    // 玩家下线钩子：清掉与该玩家相关的 Lua 定时器（否则回调会打到已下线玩家）
    void OnPlayerLogout(uint64_t playerId);

    // 供 C++ 侧读取统计
    uint64_t callCount() const { return callCount_; }
    uint64_t errorCount() const { return errorCount_; }
    size_t liveTimerCount() const { return luaTimers_.size(); }

    LuaApiCtx* apiCtx() { return ctx_; }
    void setInstructionLimit(uint64_t limit) { instructionLimit_ = limit; }

private:
    bool LoadScripts(lua_State* L);
    bool DoFile(const std::string& relPath);

    lua_State* L_ = nullptr;
    std::string root_;
    int msghRef_ = -2;    // LUA_NOREF: 错误处理器(debug.traceback)引用

    uint64_t callCount_ = 0;
    uint64_t errorCount_ = 0;

    // Lua 指令预算：Lua 里一个 while true do end 会卡死该 worker 上的所有玩家。
    // lua_pcall 无法中断，只能用 lua_sethook 做指令数上限（0 = 关闭）。
    uint64_t instructionLimit_ = 0;

    std::vector<std::string> scriptFiles_;

    LuaApiCtx* ctx_ = nullptr;

    // DB 回调登记：ctx -> lua 函数路径
    std::map<int64_t, std::string> dbPending_;
    int64_t nextCtx_ = 10000;

    // Lua 定时器登记：timerId -> {fnPath, repeat, playerId}
    struct LuaTimerInfo
    {
        std::string fnPath;
        bool repeat = false;
        uint64_t playerId = 0;
    };
    std::map<uint64_t, LuaTimerInfo> luaTimers_;
};

#endif