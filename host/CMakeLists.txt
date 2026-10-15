cmake_minimum_required(VERSION 3.16)
project(mcs_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 自研代码的告警纪律与固件一致：见的错误尽量在编译期暴露
add_compile_options(-Wall -Wextra)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets SerialPort)

# ---------------------------------------------------------------- 协议层
# 纯逻辑，不依赖 Qt、不依赖 GUI，可独立单元测试。
add_library(protocol STATIC
    src/protocol/crc16.cpp
    src/protocol/codec.cpp
)
target_include_directories(protocol PUBLIC src/protocol)

# ---------------------------------------------------------------- io 层（P0）
# 链路抽象 + 真实串口（工作线程）+ 假源回放 + 链路管理。
# 只依赖 Qt6::Core 与 Qt6::SerialPort —— **不依赖 Widgets**，所以能被无界面单测直接驱动。
add_library(hostio STATIC
    src/io/link.cpp
    src/io/link.h
    src/io/serial_link.cpp
    src/io/serial_link.h
    src/io/replay_link.cpp
    src/io/replay_link.h
    src/io/link_manager.cpp
    src/io/link_manager.h
)
target_include_directories(hostio PUBLIC src)
target_link_libraries(hostio PUBLIC protocol Qt6::Core Qt6::SerialPort)

# ---------------------------------------------------------------- domain 层（P1）
# 状态聚合与单位换算：**纯逻辑**（只依赖 Qt6::Core，不依赖 Widgets、不依赖链路实现），
# 所以能脱离界面/硬件单测 —— 这是"数值口径"唯一可信的证据来源。
add_library(domain STATIC
    src/domain/units.cpp
    src/domain/units.h
    src/domain/frames.cpp
    src/domain/frames.h
    src/domain/vehicle_state.cpp
    src/domain/vehicle_state.h
)
target_include_directories(domain PUBLIC src)
target_link_libraries(domain PUBLIC protocol Qt6::Core)

# ---------------------------------------------------------------- 界面层（P1）
# 做成**静态库**而不是直接编进 exe：这样界面可以被"无显示器"的冒烟测试直接驱动
# （tests/test_ui.cpp），而不是只能靠人肉点界面。
add_library(hostui STATIC
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/p0panel.cpp
    src/ui/p0panel.h
    src/ui/panels/overview_panel.cpp
    src/ui/panels/overview_panel.h
    src/ui/widgets/value_card.cpp
    src/ui/widgets/value_card.h
    src/ui/widgets/led_indicator.cpp
    src/ui/widgets/led_indicator.h
    src/ui/widgets/attitude_indicator.cpp
    src/ui/widgets/attitude_indicator.h
    src/ui/theme/theme.cpp
    src/ui/theme/theme.h
)
target_include_directories(hostui PUBLIC src)
target_link_libraries(hostui PUBLIC hostio domain Qt6::Widgets)

# ---------------------------------------------------------------- 上位机（装配层）
add_executable(host src/main.cpp)
target_link_libraries(host PRIVATE hostui)

# ---------------------------------------------------------------- 测试
enable_testing()
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
    add_executable(test_codec tests/test_codec.cpp)
    target_link_libraries(test_codec PRIVATE protocol Qt6::Test)
    add_test(NAME codec COMMAND test_codec)

    # P0：链路层测试（假源驱动，**不需要硬件**）
    add_executable(test_link tests/test_link.cpp)
    target_link_libraries(test_link PRIVATE hostio Qt6::Test)
    add_test(NAME link COMMAND test_link)

    # P1：domain 层测试（单位换算 / 帧解析 / 状态聚合，**不需要硬件与界面**）
    add_executable(test_domain tests/test_domain.cpp)
    target_link_libraries(test_domain PRIVATE domain hostio Qt6::Test)
    add_test(NAME domain COMMAND test_domain)

    # P1：界面冒烟测试（不需要显示器：走 offscreen 平台插件）
    add_executable(test_ui tests/test_ui.cpp)
    target_link_libraries(test_ui PRIVATE hostui Qt6::Test)
    add_test(NAME ui COMMAND test_ui)
    set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
else()
    message(STATUS "Qt6::Test 未找到，跳过单元测试目标")
endif()
