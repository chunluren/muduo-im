cmake_minimum_required(VERSION 3.10)
project(muduo-im)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

# 第三方: mymuduo-http
add_subdirectory(third_party/mymuduo-http)

# 查找依赖
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
find_library(MYSQL_LIBRARY mysqlclient)

find_path(REDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(REDIS_LIBRARY hiredis)

# nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz)
    FetchContent_MakeAvailable(json)
endif()

# 头文件
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/websocket
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/pool
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/util
    ${MYSQL_INCLUDE_DIR}
    ${REDIS_INCLUDE_DIR}
)

# gRPC 拆分骨架（Phase 1.2，#3）— 默认关闭，因开发机未必装 grpc++
# 启用方式: cmake -DWITH_GRPC=ON ..
option(WITH_GRPC "Build muduo-im-gateway / muduo-im-logic via gRPC (requires grpc++)" OFF)
if(WITH_GRPC)
    # Ubuntu 22.04 apt 装的 grpc 1.30 没出 CMake config，全走 pkg-config
    find_package(PkgConfig REQUIRED)
    # 强制走系统 .pc（/usr/lib/x86_64-linux-gnu/pkgconfig），绕开 /usr/local 的旧版
    set(ENV{PKG_CONFIG_PATH} "/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
    pkg_check_modules(PROTOBUF REQUIRED protobuf)
    pkg_check_modules(GRPCPP   REQUIRED grpc++)

    # 系统 protoc 必须用 /usr/bin（apt 装的 3.12，匹配 libprotobuf-dev），
    # 不是 /usr/local 的旧版（3.11，会 ABI 不一致）
    find_program(PROTOC protoc PATHS /usr/bin NO_DEFAULT_PATH)
    if(NOT PROTOC)
        find_program(PROTOC protoc REQUIRED)
    endif()
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
    message(STATUS "WITH_GRPC: protoc=${PROTOC} plugin=${GRPC_CPP_PLUGIN} grpc++=${GRPCPP_VERSION}")

    set(PROTO_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/proto)
    set(PROTO_FILES
        ${PROTO_DIR}/im/common.proto
        ${PROTO_DIR}/im/logic.proto
        ${PROTO_DIR}/im/registry.proto)

    set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
    file(MAKE_DIRECTORY ${PROTO_GEN_DIR}/im)

    set(GENERATED_PB_SRC "")
    foreach(pf ${PROTO_FILES})
        get_filename_component(pf_name ${pf} NAME_WE)
        set(pb_cc   ${PROTO_GEN_DIR}/im/${pf_name}.pb.cc)
        set(pb_h    ${PROTO_GEN_DIR}/im/${pf_name}.pb.h)
        set(grpc_cc ${PROTO_GEN_DIR}/im/${pf_name}.grpc.pb.cc)
        set(grpc_h  ${PROTO_GEN_DIR}/im/${pf_name}.grpc.pb.h)
        add_custom_command(
            OUTPUT ${pb_cc} ${pb_h} ${grpc_cc} ${grpc_h}
            COMMAND ${PROTOC}
                --proto_path=${PROTO_DIR}
                --cpp_out=${PROTO_GEN_DIR}
                --grpc_out=${PROTO_GEN_DIR}
                --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
                ${pf}
            DEPENDS ${pf}
            COMMENT "protoc gen ${pf_name}")
        list(APPEND GENERATED_PB_SRC ${pb_cc} ${grpc_cc})
    endforeach()

    add_library(im_proto STATIC ${GENERATED_PB_SRC})
    target_include_directories(im_proto PUBLIC ${PROTO_GEN_DIR}
        ${PROTOBUF_INCLUDE_DIRS} ${GRPCPP_INCLUDE_DIRS})
    target_link_directories(im_proto PUBLIC
        ${PROTOBUF_LIBRARY_DIRS} ${GRPCPP_LIBRARY_DIRS})
    target_link_libraries(im_proto PUBLIC ${PROTOBUF_LIBRARIES} ${GRPCPP_LIBRARIES})
    # gRPC 1.30 (Ubuntu jammy) 的 codegen 不在 .grpc.pb.cc 里 include proto_utils.h，
    # 导致 SerializationTraits 特化不可见。强行注入。INTERFACE 也带给消费者
    # （gateway / logic 直接用 grpc 模板时也需要这个特化可见）。
    target_compile_options(im_proto PUBLIC -include grpcpp/impl/codegen/proto_utils.h)

    # gateway / logic 二进制（实现暂留 stub，见 docs/plans/2026-04-25-p1-2-grpc-split-blueprint.md）
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway/main.cpp)
        add_executable(muduo-im-gateway src/gateway/main.cpp)
        target_link_libraries(muduo-im-gateway im_proto mymuduo nlohmann_json::nlohmann_json
            OpenSSL::SSL OpenSSL::Crypto ${REDIS_LIBRARY} pthread)
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/logic/main.cpp)
        add_executable(muduo-im-logic src/logic/main.cpp)
        target_link_libraries(muduo-im-logic im_proto mymuduo nlohmann_json::nlohmann_json
            OpenSSL::SSL OpenSSL::Crypto ${MYSQL_LIBRARY} ${REDIS_LIBRARY} argon2 pthread)
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/registry/main.cpp)
        add_executable(muduo-im-registry src/registry/main.cpp)
        target_link_libraries(muduo-im-registry im_proto pthread)
    endif()
endif()

# 主服务
add_executable(muduo-im src/server/main.cpp)
target_link_libraries(muduo-im
    mymuduo
    nlohmann_json::nlohmann_json
    OpenSSL::SSL OpenSSL::Crypto
    ZLIB::ZLIB
    ${MYSQL_LIBRARY}
    ${REDIS_LIBRARY}
    argon2
    pthread
)

# WebSocket benchmark client
add_executable(ws_bench benchmark/ws_bench.cpp)
target_link_libraries(ws_bench pthread)

# 测试
enable_testing()
file(GLOB TEST_SOURCES tests/*.cpp)
foreach(TEST_FILE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    # gRPC 测试只在 WITH_GRPC=ON 时编（用文件名前缀 grpc_ 区分）
    if(TEST_NAME MATCHES "^grpc_")
        if(NOT WITH_GRPC)
            continue()
        endif()
        add_executable(${TEST_NAME} ${TEST_FILE})
        target_link_libraries(${TEST_NAME} im_proto pthread)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
        continue()
    endif()
    add_executable(${TEST_NAME} ${TEST_FILE})
    target_link_libraries(${TEST_NAME}
        mymuduo
        nlohmann_json::nlohmann_json
        OpenSSL::SSL OpenSSL::Crypto
        ZLIB::ZLIB
        ${MYSQL_LIBRARY}
        ${REDIS_LIBRARY}
        argon2
        pthread
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
