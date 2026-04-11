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

# 主服务
add_executable(muduo-im src/server/main.cpp)
target_link_libraries(muduo-im
    mymuduo
    nlohmann_json::nlohmann_json
    OpenSSL::SSL OpenSSL::Crypto
    ZLIB::ZLIB
    ${MYSQL_LIBRARY}
    ${REDIS_LIBRARY}
    pthread
)
