cmake_minimum_required(VERSION 3.13)
project(MiniRocketDB)

# 开启 C++17 标准 (RocketDB 依赖)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Sanitizer 开关：默认 OFF，产出干净库（供 cgo 链接）。
# 仅当显式 -DROCKETDB_ENABLE_ASAN=ON 时才追加 AddressSanitizer，
# 且编译与链接两侧都加，避免“编了带 asan、链却不带”的错配。
# ---------------------------------------------------------------------------
option(ROCKETDB_ENABLE_ASAN "Build RocketDB with AddressSanitizer (debug only)" OFF)

if(ROCKETDB_ENABLE_ASAN)
    message(STATUS "RocketDB: AddressSanitizer ENABLED (debug build)")
    add_compile_options(-fsanitize=address -g)
    add_link_options(-fsanitize=address)
else()
    # 默认干净 release 构建：-O2，绝不包含任何 sanitizer flag
    message(STATUS "RocketDB: clean release build (-O2, no sanitizer)")
    add_compile_options(-O2)
endif()

# 头文件搜索路径
include_directories(include src)

# 收集 src 目录下所有的 .cc 文件
file(GLOB_RECURSE SOURCES "src/**/*.cc")

# 1. 编译核心静态库
add_library(rocketdb STATIC ${SOURCES})
# posix 环境依赖多线程库
target_link_libraries(rocketdb pthread)

# 2. 编译示例入口程序（当前仓库若未提供 main.cc，则仅构建库和测试）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cc")
    add_executable(db_test main.cc)
    target_link_libraries(db_test rocketdb)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(rocketdb_regression_test
        test/db_regression_test.cc
    )
    target_link_libraries(rocketdb_regression_test rocketdb)

    add_test(NAME rocketdb_regression COMMAND rocketdb_regression_test)
    set_tests_properties(rocketdb_regression PROPERTIES TIMEOUT 60)
endif()
