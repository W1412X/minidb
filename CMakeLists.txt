cmake_minimum_required(VERSION 3.20)
project(minidb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Workaround: macOS 26 beta toolchain ships incomplete modular C++ headers
# under /Library/Developer/CommandLineTools/usr/include/c++/v1.
# Force the compiler to use the SDK's own complete libc++ headers instead.
if(APPLE AND CMAKE_OSX_SYSROOT)
    set(_sdk_cxx "${CMAKE_OSX_SYSROOT}/usr/include/c++/v1")
    if(EXISTS "${_sdk_cxx}/cstdint")
        add_compile_options(-nostdinc++ "-isystem${_sdk_cxx}")
    endif()
    unset(_sdk_cxx)
endif()

# 编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

set(MINIDB_SANITIZER "" CACHE STRING "Enable sanitizer: address, undefined, or thread")
if(MINIDB_SANITIZER)
    if(MINIDB_SANITIZER STREQUAL "address")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    elseif(MINIDB_SANITIZER STREQUAL "undefined")
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
    elseif(MINIDB_SANITIZER STREQUAL "thread")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    else()
        message(FATAL_ERROR "Unsupported MINIDB_SANITIZER=${MINIDB_SANITIZER}")
    endif()
endif()

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

add_executable(minidb_pageserver src/pageserver_main.cpp)
target_link_libraries(minidb_pageserver PRIVATE minidb_lib pthread)

# 测试
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
