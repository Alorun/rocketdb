cmake_minimum_required(VERSION 3.10)
project(MiniLevelDB)

# 开启 C++17 标准 (RocketDB 依赖)
set(CMAKE_CXX_STANDARD 17)

# 头文件搜索路径
include_directories(include src)

# 收集 src 目录下所有的 .cc 文件
file(GLOB_RECURSE SOURCES "src/**/*.cc")

# 1. 编译核心静态库
add_library(rocketdb STATIC ${SOURCES})
# posix 环境依赖多线程库
target_link_libraries(rocketdb pthread)

# 2. 编译测试入口程序 (稍后创建)
add_executable(db_test main.cc)
target_link_libraries(db_test rocketdb)