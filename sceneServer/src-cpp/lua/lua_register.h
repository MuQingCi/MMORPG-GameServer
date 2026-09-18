// lua C API 注册接口(实现见 lua_bind.cpp)
#pragma once
struct lua_State;
namespace luaapi {
// 把全部 C++ API 以函数/表形式注册到 L 的全局(如 net.send / player.Moduleify_hp)
bool RegisterAll(lua_State* L);
}  // namespace luaapi
