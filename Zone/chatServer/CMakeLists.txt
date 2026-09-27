find_package(Threads REQUIRED)
add_executable(chatServer main.cc
    ${CMAKE_SOURCE_DIR}/zone_common/base/serviceApp.cc
    ${CMAKE_SOURCE_DIR}/zone_common/base/buffer.cc
    ${CMAKE_SOURCE_DIR}/zone_common/base/proto.cc
    ${CMAKE_SOURCE_DIR}/zone_common/log/logger.cc
    ${CMAKE_SOURCE_DIR}/zone_common/log/asyncLogger.cc)
target_include_directories(chatServer PRIVATE ${CMAKE_SOURCE_DIR}/zone_common)
target_link_libraries(chatServer PRIVATE Threads::Threads)
set_target_properties(chatServer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
