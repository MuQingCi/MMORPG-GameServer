#ifndef CLEARMOON_LUA_REGISTER_H
#define CLEARMOON_LUA_REGISTER_H

#include <cstdint>

struct lua_State;
struct LuaApiCtx;

namespace luaapi
{
// 把 C++ API 以函数/表形式注册到 L 的全局（net.send / db.query / timer.after / log.i ...）
// ctx 必须是该逻辑线程的上下文：绑定函数会通过 registry 取回它。
bool RegisterAll(lua_State* L, LuaApiCtx* ctx);

// registry 中保存 LuaApiCtx* 的 key（绑定层与 LuaEnv 共用）
// 注意用 "&" 前缀避免与脚本自己 setmetatable 的键冲突
constexpr const char* kApiCtxKey = "clearmoon.api_ctx";
}  // namespace luaapi

#endif

