cmake_minimum_required(VERSION 3.16)
project(mcs_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets SerialPort)

# ---------------------------------------------------------------- 协议层
# 纯逻辑，不依赖 GUI，可独立单元测试。
add_library(protocol STATIC
    src/protocol/crc16.cpp
    src/protocol/codec.cpp
)
target_include_directories(protocol PUBLIC src/protocol)

# ---------------------------------------------------------------- 上位机
add_executable(host
    src/main.cpp
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
)
target_link_libraries(host PRIVATE protocol Qt6::Widgets Qt6::SerialPort)

# ---------------------------------------------------------------- 测试
enable_testing()
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
    add_executable(test_codec tests/test_codec.cpp)
    target_link_libraries(test_codec PRIVATE protocol Qt6::Test)
    add_test(NAME codec COMMAND test_codec)
else()
    message(STATUS "Qt6::Test 未找到，跳过单元测试目标")
endif()
