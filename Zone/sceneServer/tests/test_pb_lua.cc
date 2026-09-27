#include "proto/pb_lua.h"
#include "routeTable.h"
#include "test_util.h"

#include "gameProto.pb.h"

#include <lua.hpp>
#include <string>
#include <vector>

namespace
{
// 建一个最小的 lua_State（pb_lua 只需要 state，不需要任何 C API 注册）
lua_State* NewLua()
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    return L;
}
}  // namespace

// Lua 表 -> protobuf 字节流（net.send 的入参转换路径）
TEST(PbLuaTableToBody)
{
    lua_State* L = NewLua();
    CHECK(L != nullptr);

    lua_newtable(L);
    lua_pushinteger(L, 11);
    lua_setfield(L, -2, "sx");
    lua_pushinteger(L, 22);
    lua_setfield(L, -2, "sy");
    lua_pushinteger(L, 33);
    lua_setfield(L, -2, "dx");
    lua_pushinteger(L, 44);
    lua_setfield(L, -2, "dy");

    std::string body;
    CHECK(pbl::TableToBody(L, -1, "gs.WalkReq", body));
    CHECK(!body.empty());

    gs::WalkReq req;
    CHECK(req.ParseFromArray(body.data(), static_cast<int>(body.size())));
    CHECK_EQ(req.sx(), 11);
    CHECK_EQ(req.sy(), 22);
    CHECK_EQ(req.dx(), 33);
    CHECK_EQ(req.dy(), 44);

    // 未知类型必须失败（而不是产出半个包）
    CHECK(!pbl::TableToBody(L, -1, "gs.NoSuchMessage", body));

    lua_close(L);
}

// protobuf 字节流 -> Lua 表（C2S 分发路径）
TEST(PbLuaBodyToTable)
{
    gs::WalkAck ack;
    ack.set_code(0);
    ack.set_x(100);
    ack.set_y(200);
    ack.set_dir(3);
    const std::string body = ack.SerializeAsString();

    lua_State* L = NewLua();
    CHECK(pbl::BodyToTable(L, "gs.WalkAck", body));

    lua_getfield(L, -1, "x");
    CHECK_EQ((int)lua_tointeger(L, -1), 100);
    lua_pop(L, 1);

    lua_getfield(L, -1, "dir");
    CHECK_EQ((int)lua_tointeger(L, -1), 3);
    lua_pop(L, 1);

    // 空 body：proto3 全默认值也必须能解析出表（不能报错，否则客户端永远拿不到响应）
    lua_settop(L, 0);
    CHECK(pbl::BodyToTable(L, "gs.WalkAck", ""));
    CHECK(lua_istable(L, -1));

    // 损坏的 body 必须返回失败而不是崩
    lua_settop(L, 0);
    const std::string garbage = "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF";
    (void)pbl::BodyToTable(L, "gs.WalkAck", garbage);

    lua_close(L);
}

// repeated 字段（Lua 数组 <-> proto repeated）双向转换
TEST(PbLuaRepeatedField)
{
    gs::PathAck ack;
    ack.set_code(0);
    ack.add_xs(1);
    ack.add_xs(2);
    ack.add_ys(10);
    ack.add_ys(20);
    const std::string body = ack.SerializeAsString();

    lua_State* L = NewLua();
    CHECK(pbl::BodyToTable(L, "gs.PathAck", body));

    lua_getfield(L, -1, "xs");
    CHECK(lua_istable(L, -1));
    CHECK_EQ((size_t)lua_rawlen(L, -1), (size_t)2);
    lua_rawgeti(L, -1, 2);
    CHECK_EQ((int)lua_tointeger(L, -1), 2);
    lua_settop(L, 0);

    // 回环：表 -> body -> 表
    CHECK(pbl::BodyToTable(L, "gs.PathAck", body));
    std::string back;
    CHECK(pbl::TableToBody(L, -1, "gs.PathAck", back));
    gs::PathAck parsed;
    CHECK(parsed.ParseFromArray(back.data(), static_cast<int>(back.size())));
    CHECK_EQ(parsed.xs_size(), 2);
    CHECK_EQ(parsed.xs(0), 1);
    CHECK_EQ(parsed.ys(1), 20);

    lua_close(L);
}

// 路由表自检：必须与 proto 完全一致，否则启动就该失败
TEST(RouteTableValidateAndLookup)
{
    const std::vector<std::string> errs = RouteTable::Validate();
    for (const auto& e : errs)
        std::fprintf(stderr, "route error: %s\n", e.c_str());
    CHECK(errs.empty());

    const RouteTable::Route* r = RouteTable::FindC2S(3 /*PLAYER*/, 2 /*PLAYER_MOVE*/);
    CHECK(r != nullptr);
    if (r != nullptr)
    {
        CHECK(std::string(r->req_type) == "gs.WalkReq");
        CHECK(std::string(r->ack_type) == "gs.WalkAck");
        CHECK(std::string(r->lua_fn) == "move.c2s_walk");
    }

    // 出站按消息名找（net.send 用它决定 Module/Method）
    const RouteTable::Route* byName = RouteTable::FindByTypeName("gs.RetTip");
    CHECK(byName != nullptr);
    CHECK(RouteTable::FindByTypeName("WalkAck") == r);   // 短名也要能解析
    CHECK(RouteTable::FindByTypeName("gs.Nope") == nullptr);
    CHECK(RouteTable::FindC2S(99, 99) == nullptr);
}
