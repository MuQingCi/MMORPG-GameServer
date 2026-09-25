cmake_minimum_required(VERSION 3.20)

project(ClearMoon LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 生成 compile_commands.json：clangd / 各种工具都直接吃它，
# 比手写 .clangd 里硬编码 -I 靠谱得多（旧 .clangd 里的绝对路径已经与实际目录不符）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "build type" FORCE)
endif()

# 统一警告级别：能编译但语义错的代码占当前问题的相当一部分，警告要开全
add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# 子目录
# ---------------------------------------------------------------------------
add_subdirectory(proto)        # 生成 gs.* 消息（协议是路由表与 Lua 的共同地基）
add_subdirectory(sceneServer)
add_subdirectory(gatewayServer)  # 网关：场景服/全局服/聊天服的反向连接目标

option(SCENE_BUILD_TESTS "build unit tests (sceneServer + gatewayServer)" ON)
if(SCENE_BUILD_TESTS)
    # enable_testing() 必须在所有 add_test() 之前调用（两个测试子目录都要注册用例）
    enable_testing()
    add_subdirectory(sceneServer/tests)
    add_subdirectory(gatewayServer/tests)
endif()
