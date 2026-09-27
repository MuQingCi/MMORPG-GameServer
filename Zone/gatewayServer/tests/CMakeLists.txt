# ---------------------------------------------------------------------------
# 网关单测：直接链接 gateway_core，避免重复编译源码
#   test_clientProto   客户端接入帧编解码（golden bytes / 半包粘包 / 协议不串）
#   test_gatewayRouting 会话表 / 服务注册表 / 回程路由表
#   test_gatewayConfig  双监听端口 / 后端白名单 / 迁移守卫
#   test_buffer/eventLoop/tcpConnection 既有用例
# ---------------------------------------------------------------------------
add_executable(gateway_tests
    test_main.cc
    test_asyncLogger.cc
    test_clientProto.cc
    test_gatewayConfig.cc
    test_gatewayRouting.cc
    test_buffer.cc
    test_eventLoop.cc
    test_tcpConnection.cc
)

target_include_directories(gateway_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 注入源码目录：配置用例要直接读仓库里的真实 gatewayConfig.yaml
target_compile_definitions(gateway_tests PRIVATE
    GATEWAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}/gatewayServer"
)

target_link_libraries(gateway_tests PRIVATE gateway_core)

add_test(NAME gateway_tests COMMAND gateway_tests)

# ---------------------------------------------------------------------------
# 进程级端到端工具：扮演"一个场景服 + 多个客户端"，
# 对着真进程验证两类链路（客户端帧 / 服务间帧）都能收发且被严格区分。
# 端口由 smoke 脚本通过环境变量注入，所以它自身不是 ctest 用例。
# ---------------------------------------------------------------------------
add_executable(gateway_e2e_client
    e2e_client.cc
    test_main.cc
)

target_include_directories(gateway_e2e_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gateway_e2e_client PRIVATE gateway_core)

set_target_properties(gateway_e2e_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---------------------------------------------------------------------------
# 进程启动 smoke：真的起进程、探**两个**端口、SIGTERM 收尾
#   （单测覆盖不到"构建产物能起来"，而它是阶段 0 的完成判据）
# ---------------------------------------------------------------------------
add_test(NAME gateway_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_gateway.sh
            $<TARGET_FILE:gatewayServer>
            ${CMAKE_CURRENT_BINARY_DIR}
)

# 端到端 smoke：公网链路 + 内网链路 + 协议区分 + 鉴权 + 回程路由
add_test(NAME gateway_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_gateway_e2e.sh
            $<TARGET_FILE:gatewayServer>
            $<TARGET_FILE:gateway_e2e_client>
            ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# 真实链路端到端：真 gateway + 真 sceneServer + 真客户端帧（WalkReq/WalkAck）
#   python3 只用于扮演客户端（标准库、不链接项目代码），因此找不到就跳过该用例
# ---------------------------------------------------------------------------
find_program(PYTHON3_EXECUTABLE NAMES python3)
if(PYTHON3_EXECUTABLE AND TARGET sceneServer)
    add_test(NAME gateway_real_scene
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/smoke_gateway_real_scene.sh
                $<TARGET_FILE:gatewayServer>
                $<TARGET_FILE:sceneServer>
                ${CMAKE_SOURCE_DIR}/gatewayServer
                ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(gateway_real_scene PROPERTIES TIMEOUT 120)
else()
    message(STATUS "gateway: python3/sceneServer 不可用，跳过 gateway_real_scene 用例")
endif()

if(PYTHON3_EXECUTABLE AND TARGET chatServer AND TARGET globalServer)
    add_test(NAME gateway_chat_global
        COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_chat_global.py
                $<TARGET_FILE:gatewayServer> $<TARGET_FILE:chatServer>
                $<TARGET_FILE:globalServer>)
    set_tests_properties(gateway_chat_global PROPERTIES TIMEOUT 30)
endif()

# 既有 shell smoke 用例共用 CMAKE_CURRENT_BINARY_DIR，不能并行清理/重写其日志目录。
set_tests_properties(gateway_smoke gateway_e2e PROPERTIES RUN_SERIAL TRUE)
if(PYTHON3_EXECUTABLE AND TARGET sceneServer)
    set_tests_properties(gateway_real_scene PROPERTIES RUN_SERIAL TRUE)
endif()
