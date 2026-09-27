# ---------------------------------------------------------------------------
# 单元测试：直接链接 scene_core，不需要重复编译源码
# ---------------------------------------------------------------------------
add_executable(scene_tests
    test_main.cc
    test_buffer.cc
    test_proto.cc
    test_msgBus.cc
    test_timer.cc
    test_config.cc
    test_pb_lua.cc
    test_redisKey.cc
    test_lua_redis.cc
    test_logicThread.cc
)

target_include_directories(scene_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/sceneServer/src-cpp
)

# 注入源码目录：配置测试要直接读仓库里的真实 yaml
# （这样"yaml 用了 Tab""键名拼错""字段没配全"会在测试期就暴露，而不是等部署时）
target_compile_definitions(scene_tests PRIVATE
    SCENE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/sceneServer"
)

target_link_libraries(scene_tests PRIVATE scene_core)

add_test(NAME scene_tests COMMAND scene_tests)

# ---------------------------------------------------------------------------
# 进程级 smoke：无效路由的消息不得创建 Actor（阶段 1 任务 1.16 的回归守卫）
#   覆盖单测触不到的链路：真实网络帧 → 网络线程 → MsgBus → 逻辑线程 → 路由校验
# ---------------------------------------------------------------------------
add_test(NAME scene_smoke_invalid_route
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_scene_invalid_route.sh
            $<TARGET_FILE:sceneServer>
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_SOURCE_DIR}/sceneServer
)
