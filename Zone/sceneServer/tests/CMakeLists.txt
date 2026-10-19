# ---------------------------------------------------------------------------
# 单元测试：直接链接 scene_core，不需要重复编译源码
# ---------------------------------------------------------------------------
add_executable(scene_tests
    test_main.cc
    test_buffer.cc
    test_proto.cc
    test_msgBus.cc
    test_taskDelivery.cc
    test_playerSnapshot.cc
    test_playerEconomy.cc
    test_playerPersistence.cc
    test_dbTransaction.cc
    test_timer.cc
    test_config.cc
    test_pb_lua.cc
    test_redisKey.cc
    test_lua_redis.cc
    test_logicThread.cc
    test_sceneMetrics.cc
    test_sceneMonitoring.cc
    test_gatewayBinding.cc
    test_workerPendingTasks.cc
    test_aoiRouting.cc
    test_aoiService.cc
    test_aoiClientView.cc
    test_aoiReplica.cc
    test_aoiViewSet.cc
    test_lua_aoi.cc
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

add_executable(config_diagnostics_test test_main.cc test_config.cc)
target_include_directories(config_diagnostics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(config_diagnostics_test PRIVATE SCENE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/sceneServer")
target_compile_options(config_diagnostics_test PRIVATE -UNDEBUG)
target_link_libraries(config_diagnostics_test PRIVATE scene_core)

add_executable(work_diagnostics_test test_work_diagnostics.cc)
target_link_libraries(work_diagnostics_test PRIVATE scene_core)
target_compile_options(work_diagnostics_test PRIVATE -UNDEBUG)
add_test(NAME work_diagnostics_test COMMAND work_diagnostics_test)

add_executable(busy_diagnostics_test test_main.cc test_workerPendingTasks.cc test_sceneMonitoring.cc test_taskDelivery.cc)
target_include_directories(busy_diagnostics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(busy_diagnostics_test PRIVATE scene_core)
target_compile_options(busy_diagnostics_test PRIVATE -UNDEBUG)

add_executable(db_scan_attribution_bench db_scan_attribution_bench.cc)
target_link_libraries(db_scan_attribution_bench PRIVATE scene_core)
target_compile_options(db_scan_attribution_bench PRIVATE -UNDEBUG)

add_executable(aoi_diagnostics_test test_aoiDiagnostics.cc)
target_link_libraries(aoi_diagnostics_test PRIVATE scene_core)
target_compile_options(aoi_diagnostics_test PRIVATE -UNDEBUG)
add_test(NAME aoi_diagnostics_test COMMAND aoi_diagnostics_test)

add_executable(aoi_hotspot_bench aoi_hotspot_bench.cc)
target_link_libraries(aoi_hotspot_bench PRIVATE scene_core)
target_compile_definitions(aoi_hotspot_bench PRIVATE SCENE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/sceneServer")

# 显式执行的隔离真实数据库验收，不连接配置中的业务数据库。
add_executable(persistence_dao_probe persistence_dao_probe.cc)
target_link_libraries(persistence_dao_probe PRIVATE scene_core)

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
set_tests_properties(scene_smoke_invalid_route PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
