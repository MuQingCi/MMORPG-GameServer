# ---------------------------------------------------------------------------
# 协议生成：把 proto/*.proto 编译成 C++（gs 命名空间）
# ---------------------------------------------------------------------------
find_package(Protobuf REQUIRED)     # module 模式：提供 protoc + protobuf_generate_cpp
find_package(PkgConfig QUIET)

# 链接：优先用 pkg-config 的 protobuf.pc。
# 原因：新版 protobuf 的运行时依赖 absl（几十个子库），只链 libprotobuf.so 会在链接期
# 报 "DSO missing from command line"（symbol 其实在 libabsl_*.so 里）。
# pkg-config 会把这些传递依赖一并给出，是最省事且可移植的做法。
if(PkgConfig_FOUND)
    pkg_check_modules(PROTOBUF_PC QUIET protobuf)
endif()

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
    gameProto.proto
)

add_library(gs_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})

target_include_directories(gs_proto PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}      # 让 #include "gameProto.pb.h" 生效
)

if(PROTOBUF_PC_FOUND)
    target_include_directories(gs_proto PUBLIC ${PROTOBUF_PC_INCLUDE_DIRS})
    target_link_directories(gs_proto PUBLIC ${PROTOBUF_PC_LIBRARY_DIRS})
    target_link_libraries(gs_proto PUBLIC ${PROTOBUF_PC_LINK_LIBRARIES})
    message(STATUS "gs_proto: protobuf deps from pkg-config (${PROTOBUF_PC_VERSION})")
else()
    target_link_libraries(gs_proto PUBLIC ${Protobuf_LIBRARIES})
    message(STATUS "gs_proto: protobuf deps from FindProtobuf")
endif()

set_target_properties(gs_proto PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

