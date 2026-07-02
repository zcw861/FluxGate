# FluxGate 顶层构建脚本：定义版本、C++ 标准、核心库、主程序、测试和安装规则。
cmake_minimum_required(VERSION 3.16)
project(FluxGate VERSION 1.2.0 LANGUAGES CXX)

# 是否构建 CTest 单元测试。发布环境只需要二进制时可设置为 OFF。
option(FLUXGATE_BUILD_TESTS "Build FluxGate unit tests" ON)

# Debug 排错开关：同时启用 AddressSanitizer 与 UndefinedBehaviorSanitizer。
option(FLUXGATE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# 项目严格使用标准 C++11，不依赖 GNU 扩展，保证构建约束与简历描述一致。
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出 compile_commands.json，便于 clangd、静态分析和 IDE 获取真实编译参数。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 加载项目自定义 CMake 模块，其中统一维护 GCC/Clang/MSVC 告警选项。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# std::thread 在 Linux 上需要 pthread；使用导入目标保证编译和链接参数正确。
find_package(Threads REQUIRED)

# 核心逻辑构建为静态库，使主程序和测试程序复用同一套实现。
add_library(fluxgate_core
    src/Backend.cpp
    src/BackendPool.cpp
    src/Config.cpp
    src/HealthChecker.cpp
    src/Logger.cpp
    src/Metrics.cpp
    src/Net.cpp
    src/ProxySession.cpp
    src/Worker.cpp
)

# PUBLIC 表示使用核心库的目标也能自动获得 include/ 路径和线程依赖。
target_include_directories(fluxgate_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fluxgate_core PUBLIC Threads::Threads)
fluxgate_set_warnings(fluxgate_core)

# Sanitizer 参数必须同时加入编译和链接阶段，否则运行库不会被正确链接。
if(FLUXGATE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fluxgate_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(fluxgate_core PUBLIC -fsanitize=address,undefined)
endif()

# 主程序只保留进程生命周期和 Acceptor，网络核心来自 fluxgate_core。
add_executable(fluxgate src/main.cpp)
target_link_libraries(fluxgate PRIVATE fluxgate_core)
fluxgate_set_warnings(fluxgate)

# cmake --install 可安装可执行文件与默认配置。
install(TARGETS fluxgate RUNTIME DESTINATION bin)
install(FILES config/fluxgate.conf DESTINATION etc/fluxgate)

# 单元测试默认开启，并注册到 CTest。
if(FLUXGATE_BUILD_TESTS)
    enable_testing()
    add_executable(fluxgate_tests tests/fluxgate_tests.cpp)
    target_link_libraries(fluxgate_tests PRIVATE fluxgate_core)

    # 测试通过该宏定位源码目录中的 tests/test.conf。
    target_compile_definitions(fluxgate_tests PRIVATE FLUXGATE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    fluxgate_set_warnings(fluxgate_tests)
    add_test(NAME fluxgate_tests COMMAND fluxgate_tests)
endif()
