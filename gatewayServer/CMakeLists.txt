# ---------------------------------------------------------------------------
# gatewayServer：网关的 Reactor 网络层（epoll + 非阻塞 IO + 多 IO 线程）
#
# 组织方式与 sceneServer 保持一致：
#   gateway_core（静态库，除 main.cpp 外的全部实现）
#   gatewayServer（可执行，只含 main.cpp）
# 这样单元测试可以直接链接 gateway_core，而不必把源码再编一遍。
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

set(GATEWAY_CORE_SOURCES
    acceptor.cc
    gatewayConfig.cc
    gatewayServer.cc
    inetAddress.cc
    socket.cc
    tcpConnection.cc

    common/currentThread.cc
    common/timestamp.cc

    event/channel.cc
    event/eventLoop.cc
    event/eventThread.cc
    event/eventThreadPool.cc

    poller/epoller.cc
    poller/poller.cc

    timer/timer.cc
    timer/timerQueue.cc
)

add_library(gateway_core STATIC ${GATEWAY_CORE_SOURCES})

# ---------------------------------------------------------------------------
# include 根：
#   1) gatewayServer/ —— 本服务自己的头文件（acceptor.h、event/*、poller/*、timer/*）
#   2) zone_common/   —— 公共实现（common/buffer.h、common/yaml.h、log/logger.h ...）
#
# 顺序：自己的在前。若两边出现同名头文件，解析结果会确定地命中自己的那份，
# 而不是取决于编译器/链接器的偶然顺序。
# （给公共库加稳定前缀 zone/base/... 是阶段 2 的目标，届时这里只留 target_link_libraries）
# ---------------------------------------------------------------------------
target_include_directories(gateway_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/zone_common
)

# ---------------------------------------------------------------------------
# 公共库（zone_common）：阶段 2 会抽成独立的 zone_base / zone_log target。
# 现阶段先按源文件直接编入 gateway_core，只取网关实际用到的部分：
#   Buffer（收发缓冲）、YAML（配置解析）、Logger/AsyncLogger（日志）
#
# 刻意**不编** zone_common/common/json.cc 与 proto.cc：
#   - json 网关当前不使用；
#   - proto.cc 属于 32B 旧协议世代（协议在阶段 3 重新定稿），
#     现在把它引进网关等于让旧协议在新代码里生根。
# ---------------------------------------------------------------------------
target_sources(gateway_core PRIVATE
    ${CMAKE_SOURCE_DIR}/zone_common/common/buffer.cc
    ${CMAKE_SOURCE_DIR}/zone_common/common/yaml.cc
    ${CMAKE_SOURCE_DIR}/zone_common/log/logger.cc
    ${CMAKE_SOURCE_DIR}/zone_common/log/asyncLogger.cc
)

target_link_libraries(gateway_core PUBLIC
    Threads::Threads
)

target_compile_options(gateway_core PRIVATE -Werror=return-type)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(gatewayServer main.cpp)
target_link_libraries(gatewayServer PRIVATE gateway_core)

# 输出到 build/bin：与 sceneServer 一致，便于统一启动/冒烟
set_target_properties(gatewayServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
