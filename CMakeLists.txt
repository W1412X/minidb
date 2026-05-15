cmake_minimum_required(VERSION 3.20)
project(minidb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# 收集所有源文件 (不含 main.cpp)
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS
    src/common/*.cpp
    src/container/*.cpp
    src/storage/*.cpp
    src/record/*.cpp
    src/catalog/*.cpp
    src/sql/*.cpp
    src/index/*.cpp
    src/database/*.cpp
    src/repl/*.cpp
    src/transaction/*.cpp
    src/recovery/*.cpp
    src/network/*.cpp
    src/concurrency/*.cpp
)

# 静态库
add_library(minidb_lib STATIC ${LIB_SOURCES})
target_include_directories(minidb_lib PUBLIC src/)

# 主可执行文件
add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE minidb_lib pthread)

# 测试
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
