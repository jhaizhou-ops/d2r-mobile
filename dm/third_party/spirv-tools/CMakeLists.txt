# SPIRV-Tools 的优化器库（内联三步要用），只编库本身，不编工具和测试；指令表已预生成在 generated/，构建不跑 Python。
# 来源与改动见 README.md。
# The SPIRV-Tools optimizer library (needed for the three inlining passes): the library only, no tools or tests; the
# instruction tables are pre-generated in generated/, so the build runs no Python. Origin and changes: README.md.
cmake_minimum_required(VERSION 3.22)
project(dm_spirv_tools LANGUAGES CXX)
set(S ${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB DM_SPIRV_TOOLS_SRCS ${S}/source/*.cpp ${S}/source/util/*.cpp ${S}/source/val/*.cpp ${S}/source/opt/*.cpp)
add_library(dm_spirv_opt STATIC ${DM_SPIRV_TOOLS_SRCS})
target_include_directories(dm_spirv_opt PUBLIC ${S}/include PRIVATE ${S} ${S}/generated ${S}/spirv-headers/include)
target_compile_features(dm_spirv_opt PUBLIC cxx_std_17)
# -g0：安卓包保留 .so 的调试信息（keepDebugSymbols），这个库带上就从 8 MB 涨到 200 MB 以上；函数名仍在符号表里。
# -g0: the Android package keeps the .so debug info (keepDebugSymbols), which would take this library from 8 MB past 200 MB;
# function names stay in the symbol table.
target_compile_options(dm_spirv_opt PRIVATE -O2 -g0 -fno-rtti -w)
set_target_properties(dm_spirv_opt PROPERTIES POSITION_INDEPENDENT_CODE ON)
