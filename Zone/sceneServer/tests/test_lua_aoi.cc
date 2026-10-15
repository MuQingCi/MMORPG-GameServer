#include "test_util.h"
#include "lua/lua_register.h"
#include "lua/luaApiCtx.h"
#include "player/playerService.h"
#include <lua.hpp>
#include <stdexcept>

namespace {
struct LocalSink : AoiPushSink {
    AoiPushResult sendToClient(const AoiClientPushRequest&) override { return AoiPushResult::kAccepted; }
    AoiWorkerPublishResult publishToWorkers(const AoiWorkerPublishRequest&) override { return {}; }
};
Map LuaAoiMap() {
    Map map; std::string err;
    if(!Map::Load(std::string(SCENE_SOURCE_DIR)+"/config/map.yaml",50,map,err)) throw std::runtime_error(err);
    return map;
}
}

TEST(LuaPlayerActionsNotifyAoiWithoutScriptAoiCalls)
{
    auto map = LuaAoiMap(); AoiConfig config; LocalSink sink;
    PlayerManager players(0); AoiService aoi(0,config,sink,map);
    PlayerService service(players,aoi,map);
    uint32_t epoch;
    auto* player = players.getOrCreate(100,1,epoch);
    player->clearDirty();
    LuaApiCtx ctx; ctx.players = &players; ctx.playerService = &service;
    lua_State* L = luaL_newstate(); luaL_openlibs(L);
    CHECK(luaapi::RegisterAll(L,&ctx));
    CHECK_EQ(luaL_dostring(L,"assert(not player.move(100,20,20)); assert(not player.enter_scene(100,-1,20))"),0);
    CHECK_EQ(player->x(),0);
    CHECK(!player->dirty());
    CHECK_EQ(player->mutationRevision(),0u);
    CHECK(aoi.BuildOwnerSnapshot().ents.empty());
    CHECK_EQ(luaL_dostring(L,"assert(player.enter_scene(100,10,10)); assert(not player.enter_scene(100,20,20))"),0);
    CHECK_EQ(aoi.BuildOwnerSnapshot().ents.size(),1u);
    CHECK_EQ(player->mutationRevision(),1u);
    CHECK_EQ(luaL_dostring(L,"assert(player.move(100,20,20,-1))"),0);
    auto snapshot = aoi.BuildOwnerSnapshot();
    CHECK_EQ(snapshot.ents[0].x,20u);
    CHECK_EQ(snapshot.ents[0].dir,-1);
    CHECK_EQ(snapshot.ents[0].spatialVersion,2u);
    CHECK_EQ(player->mutationRevision(),2u);
    CHECK_EQ(luaL_dostring(L,"assert(not player.move(100,2000,20)); assert(not player.move(100,4294967296,20))"),0);
    CHECK_EQ(player->x(),20);
    CHECK_EQ(aoi.BuildOwnerSnapshot().seq,snapshot.seq);
    CHECK_EQ(player->mutationRevision(),2u);
    // 后续 Lua 错误不会撤销已实际发生并通知 AOI 的移动。
    CHECK(luaL_dostring(L,"assert(player.move(100,21,20)); error('after move')") != 0);
    lua_pop(L,1);
    CHECK_EQ(aoi.BuildOwnerSnapshot().ents[0].x,21u);
    CHECK_EQ(luaL_dostring(L,"assert(player.leave_scene(100)); assert(not player.move(100,22,20)); assert(player.leave_scene(100))"),0);
    CHECK(aoi.BuildOwnerSnapshot().ents.empty());
    CHECK_EQ(player->x(),21);
    CHECK_EQ(player->mutationRevision(),3u);
    CHECK_EQ(luaL_dostring(L,"assert(player.enter_scene(100,30,30))"),0);
    CHECK_EQ(aoi.BuildOwnerSnapshot().ents[0].entityGeneration,2u);
    CHECK_EQ(service.LeaveScene(100),PlayerService::Result::kOk);
    CHECK(players.remove(100));
    CHECK(aoi.BuildOwnerSnapshot().ents.empty());
    lua_close(L);
}