# ---------------------------------------------------------------------------
# sceneServer：核心逻辑编成静态库 scene_core，可执行文件只留 main.cc
#   这样单测可以直接链接 scene_core（不然测试只能重复编译一遍源码）
# ---------------------------------------------------------------------------
set(SCENE_CORE_SOURCES
    src-cpp/sceneServer.cc
    src-cpp/routeTable.cc

    src-cpp/common/msgBus.cc

    # ---------------------------------------------------------------------
    # 公共实现（zone_common）：Buffer/序列化/配置解析/日志
    #
    # 背景：这些文件原先在 sceneServer/src-cpp/common 与 src-cpp/log 下各有一份
    # 私有副本，现已统一迁到 zone_common/。这里直接引用公共实现，
    # **不再保留 scene 侧的同名副本**（否则就是"两套实现 + 实现漂移"）。
    #
    # 阶段 2 会把它们做成独立的 zone_base / zone_log target（带稳定 include 前缀），
    # 届时这里只留 target_link_libraries；现阶段先按源文件直接编入，
    # 目的是让 scene_core 恢复可构建、可验证。
    # ---------------------------------------------------------------------
    ${CMAKE_SOURCE_DIR}/zone_common/base/buffer.cc
    ${CMAKE_SOURCE_DIR}/zone_common/base/json.cc
    ${CMAKE_SOURCE_DIR}/zone_common/base/proto.cc
    ${CMAKE_SOURCE_DIR}/zone_common/base/yaml.cc
    ${CMAKE_SOURCE_DIR}/zone_common/log/logger.cc
    ${CMAKE_SOURCE_DIR}/zone_common/log/asyncLogger.cc

    src-cpp/config/sceneConfig.cc
    src-cpp/config/zoneConfig.cc

    src-cpp/db/dbThread.cc
    src-cpp/db/dbValue.cc
    src-cpp/db/mysqlConnection.cc
    src-cpp/db/redisConnection.cc
    src-cpp/db/redisKey.cc

    src-cpp/logic/logicThread.cc

    src-cpp/lua/luaBind.cc
    src-cpp/lua/luaEnv.cc

    src-cpp/net/net_server.cc

    src-cpp/player/bag.cc
    src-cpp/player/player.cc
    src-cpp/player/playerManager.cc

    src-cpp/proto/pb_lua.cc

    src-cpp/timer/TimerThread.cc
)

add_library(scene_core STATIC ${SCENE_CORE_SOURCES})

# include 根：
#   1) src-cpp        —— 场景服自身的头文件（common/msg.h、config/*、net/* ...）
#   2) zone_common    —— 公共实现（base/buffer.h、base/proto.h、log/logger.h ...）
#
# 顺序说明：src-cpp 在前。若将来某条路径在两边都存在同名头文件，
# 解析结果会**确定地**命中场景服自己那份，而不是依赖链接器的偶然性。
# （"只暴露一个 include 根"是阶段 2 的目标：届时公共库有稳定前缀 zone/base/...）
target_include_directories(scene_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src-cpp
    ${CMAKE_SOURCE_DIR}/zone_common
)

target_link_libraries(scene_core PUBLIC
    gs_proto
    Threads::Threads
)

# ---------------------------------------------------------------------------
# 依赖：Lua / MySQL / Redis 客户端
#   三者都做"找不到就降级"的处理，保证只装了一半依赖的开发机也能编过并启动，
#   只是对应通道会在运行时明确报"not compiled in / not connected"。
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# ---- Lua ----
if(PkgConfig_FOUND)
    pkg_check_modules(LUA QUIET lua5.5 lua5.4 lua5.3 lua)
endif()
if(NOT LUA_FOUND)
    find_path(LUA_INCLUDE_DIR lua.hpp PATH_SUFFIXES lua5.5 lua5.4 lua5.3)
    find_library(LUA_LIBRARY NAMES lua5.5 lua5.4 lua5.3 lua)
else()
    set(LUA_INCLUDE_DIR ${LUA_INCLUDE_DIRS})
    set(LUA_LIBRARY ${LUA_LINK_LIBRARIES})
endif()

if(LUA_INCLUDE_DIR AND LUA_LIBRARY)
    target_include_directories(scene_core PUBLIC ${LUA_INCLUDE_DIR})
    target_link_libraries(scene_core PUBLIC ${LUA_LIBRARY})
    message(STATUS "sceneServer: lua found at ${LUA_LIBRARY}")
else()
    message(FATAL_ERROR "sceneServer: lua not found (脚本层是场景服的核心，不能降级)")
endif()

# ---- MySQL (MariaDB Connector/C) ----
find_path(MYSQL_INCLUDE_DIR mysql.h
    PATHS /usr/include/mysql /usr/local/include/mysql /opt/homebrew/include/mysql
)
find_library(MYSQL_LIBRARY NAMES mariadb mariadbclient mysqlclient mysqlclient_r
    PATHS /usr/lib /usr/local/lib
)

if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
    target_include_directories(scene_core PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(scene_core PUBLIC ${MYSQL_LIBRARY})
    target_compile_definitions(scene_core PUBLIC SCENE_HAS_MYSQL=1)
    message(STATUS "sceneServer: mysql found at ${MYSQL_LIBRARY}")
else()
    target_compile_definitions(scene_core PUBLIC SCENE_HAS_MYSQL=0)
    message(WARNING "sceneServer: mysql not found, MySQL channel will be disabled")
endif()

# ---- Redis (valkey 或 hiredis) ----
find_path(VALKEY_INCLUDE_DIR valkey/valkey.h PATHS /usr/local/include /usr/include /opt/homebrew/include)
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h PATHS /usr/local/include /usr/include /opt/homebrew/include)

set(SCENE_REDIS_FOUND FALSE)

if(VALKEY_INCLUDE_DIR)
    find_library(VALKEY_LIBRARY NAMES valkey PATHS /usr/local/lib /usr/lib /opt/homebrew/lib)
    if(VALKEY_LIBRARY)
        target_include_directories(scene_core PRIVATE ${VALKEY_INCLUDE_DIR})
        target_link_libraries(scene_core PUBLIC ${VALKEY_LIBRARY})
        target_compile_definitions(scene_core PUBLIC SCENE_HAS_REDIS=1 SCENE_USE_VALKEY=1)
        set(SCENE_REDIS_FOUND TRUE)
        message(STATUS "sceneServer: valkey found at ${VALKEY_LIBRARY}")
    endif()
elseif(HIREDIS_INCLUDE_DIR)
    find_library(HIREDIS_LIBRARY NAMES hiredis PATHS /usr/local/lib /usr/lib /opt/homebrew/lib)
    if(HIREDIS_LIBRARY)
        target_include_directories(scene_core PRIVATE ${HIREDIS_INCLUDE_DIR})
        target_link_libraries(scene_core PUBLIC ${HIREDIS_LIBRARY})
        target_compile_definitions(scene_core PUBLIC SCENE_HAS_REDIS=1 SCENE_USE_VALKEY=0)
        set(SCENE_REDIS_FOUND TRUE)
        message(STATUS "sceneServer: hiredis found at ${HIREDIS_LIBRARY}")
    endif()
endif()

if(NOT SCENE_REDIS_FOUND)
    target_compile_definitions(scene_core PUBLIC SCENE_HAS_REDIS=0 SCENE_USE_VALKEY=0)
    message(WARNING "sceneServer: no redis client found, Redis channel will be disabled")
endif()

target_compile_options(scene_core PRIVATE -Werror=return-type)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(sceneServer main.cc)
target_link_libraries(sceneServer PRIVATE scene_core)

# 输出到 build/bin：方便在源码目录直接运行时用相对路径找配置与脚本
set_target_properties(sceneServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
