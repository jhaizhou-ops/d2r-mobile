# 安卓宿主构建（NDK，arm64-v8a）。源文件清单不手抄：从 dm/host/Makefile 的 SRC / GFX / LIBDEFLATE 取（与 iOS 的 ios/tools/gen_build.py 同一思路），
# 排除 Mac 专用（*_mac.*）、Objective-C（*.m，安卓另有实现）与 main.c（单独编：命令行 dmhost 用原名 main，App 的 libdm.so 以 -Dmain=dm_main 编）。
# 汇编（Mach-O 写法）经 tools/asm_elf.py 转 ELF 后编；compat/ 只在安卓构建的头文件路径里，苹果构建看不到。
#
# 必需：-DDM_GEN_DIR=<目录>，内含由游戏文件生成的 dm_vault_tab.h / dm_hud_exp.h / dm_bright_levels.inc（生成方法同 dm/host/Makefile 对应规则，不入库）。
# 产物：dmhost（命令行，adb shell 跑，开发验证用）；libdm.so（App 的 JNI 宿主）。
#
# Android host build (NDK, arm64-v8a). The source list is not copied by hand: it is taken from SRC / GFX / LIBDEFLATE in dm/host/Makefile (the same idea as iOS's ios/tools/gen_build.py),
# excluding Mac-only files (*_mac.*), Objective-C (*.m, Android has its own implementations) and main.c (built separately: the command-line dmhost keeps the name main, the App's libdm.so is built with -Dmain=dm_main).
# Assembly (Mach-O syntax) is converted to ELF by tools/asm_elf.py before assembling; compat/ is only on the Android build's header path, invisible to Apple builds.
#
# Required: -DDM_GEN_DIR=<directory> containing dm_vault_tab.h / dm_hud_exp.h / dm_bright_levels.inc generated from the game files (generated as by the matching rules in dm/host/Makefile, not committed).
# Outputs: dmhost (command line, run from adb shell, for development checks); libdm.so (the App's JNI host).
cmake_minimum_required(VERSION 3.22)
project(dm_android C CXX ASM)
# 产物里不留本机路径（断言带的 __FILE__、调试信息里的源文件 / 生成目录 / NDK 路径都会编进 libdm.so，暴露用户名与目录结构）：
# 一律换成中性前缀。长的在前（构建目录在源码树里面）。发布包核对见 tools/shell_apk.sh。
# No local paths in the products (__FILE__ in assertions and the source / generated / NDK paths in debug info all end up in libdm.so,
# exposing the user name and directory layout): replace them with neutral prefixes, longer ones first (the build directory is inside
# the source tree). The release package check is in tools/shell_apk.sh.
get_filename_component(DM_PATH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
foreach(map "${CMAKE_BINARY_DIR}=build" "${DM_GEN_DIR}=gen" "${CMAKE_ANDROID_NDK}=ndk" "${DM_PATH_ROOT}=src")
  add_compile_options("-ffile-prefix-map=${map}")
endforeach()
add_subdirectory(../third_party/spirv-tools ${CMAKE_CURRENT_BINARY_DIR}/spirv-tools)   # 先于引擎：Adreno 750 载入时内联辅助函数 | before the engine: Adreno 750 helper inlining at load
add_subdirectory(../gfx/vulkan ${CMAKE_CURRENT_BINARY_DIR}/vulkan)

set(DM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(HOST ${DM}/host)
if(NOT DM_GEN_DIR OR NOT EXISTS ${DM_GEN_DIR}/dm_vault_tab.h)
  message(FATAL_ERROR "要 -DDM_GEN_DIR=<由游戏文件生成的宿主表头目录>（dm_vault_tab.h / dm_hud_exp.h / dm_bright_levels.inc，见 dm/host/Makefile）")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# App 与资源共享公开生成源码的指纹；输入清单由同一工具提供，变动会重新生成头文件。
# The app and resources share a fingerprint of public generator inputs; one tool supplies the dependencies.
set(DM_OVERLAY_TOOL ${DM}/ios/tools/overlay_contract.py)
get_filename_component(DM_REPO_ROOT ${DM}/.. ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DM_OVERLAY_TOOL})
execute_process(COMMAND ${Python3_EXECUTABLE} ${DM_OVERLAY_TOOL} --inputs
  OUTPUT_VARIABLE overlay_inputs RESULT_VARIABLE overlay_result OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT overlay_result EQUAL 0)
  message(FATAL_ERROR "Cannot read overlay contract dependencies")
endif()
string(REPLACE "\r\n" "\n" overlay_inputs "${overlay_inputs}")
string(REPLACE "\n" ";" overlay_inputs "${overlay_inputs}")
list(TRANSFORM overlay_inputs PREPEND "${DM_REPO_ROOT}/")
set(DM_OVERLAY_HEADER ${CMAKE_CURRENT_BINARY_DIR}/dm_overlay_contract.h)
add_custom_command(OUTPUT ${DM_OVERLAY_HEADER}
  COMMAND ${Python3_EXECUTABLE} ${DM_OVERLAY_TOOL} --repo-root ${DM_REPO_ROOT} --header ${DM_OVERLAY_HEADER}
  DEPENDS ${overlay_inputs} VERBATIM)

# Python 保留 Makefile 续行边界，避免 CMake 列表的反斜杠转义吞掉源码行。
# Python preserves Makefile continuations instead of CMake list escaping joining them.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HOST}/Makefile ${CMAKE_CURRENT_SOURCE_DIR}/tools/host_sources.py)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/host_sources.py
  ${HOST}/Makefile ${CMAKE_CURRENT_BINARY_DIR}/host_sources.cmake
  RESULT_VARIABLE parse_result)
if(NOT parse_result EQUAL 0)
  message(FATAL_ERROR "Cannot parse host sources for Android")
endif()
include(${CMAKE_CURRENT_BINARY_DIR}/host_sources.cmake)
file(GLOB HLE_SRCS CONFIGURE_DEPENDS ${HOST}/hle_*.c)
file(GLOB GFX_SRCS CONFIGURE_DEPENDS ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)

# ---- 汇编：Mach-O → ELF | Assembly: Mach-O → ELF ----
set(ASM_SRCS)
foreach(s ${HOST}/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
  get_filename_component(n ${s} NAME)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/asm/${n})
  add_custom_command(OUTPUT ${out} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py ${s} ${out}
                     DEPENDS ${s} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py VERBATIM)
  list(APPEND ASM_SRCS ${out})
endforeach()

set(ANDROID_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/front_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/touch_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_present.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/clonefile.c
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/digest.c
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/mach_thread.c
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/mach_symbols.c
  ${DM}/gfx/vulkan/nvn_vk.c)

# 技能索引由玩家资源在仓外生成；公共只读桥复用同一套身份与过期校验。
# Generate the skill index outside the repository from the player's files; share identity and freshness checks.
if(NOT EXISTS ${DM_GEN_DIR}/dm_touch_native_art.c)
  message(FATAL_ERROR "Generate native_art/generate.py <romfs> --output-dir <DM_GEN_DIR> first")
endif()
list(APPEND ANDROID_SRCS
  ${DM}/ios/Sources/dm_touch_game_state.c
  ${DM}/ios/Sources/dm_touch_game_identity.c
  ${DM}/ios/Sources/dm_touch_game_bridge.c
  ${DM_GEN_DIR}/dm_touch_native_art.c)

add_library(dmcore STATIC ${HOST_SRCS} ${HLE_SRCS} ${GFX_SRCS} ${LIBDEFLATE_SRCS} ${ASM_SRCS} ${ANDROID_SRCS})
list(REMOVE_DUPLICATES HOST_SRCS)
target_include_directories(dmcore PUBLIC ${HOST} ${DM}/gfx/nvn/include ${CMAKE_CURRENT_SOURCE_DIR}/compat/include ${DM_GEN_DIR})
target_include_directories(dmcore PUBLIC ${DM}/ios/Sources)
# 与 Makefile 同档优化（-O1 -g）；dm_android_compat.h 强制包含；-fno-strict-aliasing 同苹果 clang 缺省行为下宿主对客体内存的读写习惯
# Same optimization level as the Makefile (-O1 -g); dm_android_compat.h force-included; -fno-strict-aliasing matches the host's habit of reading and writing guest memory under Apple clang's default behaviour
set(DM_CFLAGS -O1 -g -Wall -Wno-unused-function -fno-strict-aliasing -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/include/dm_android_compat.h)
target_compile_options(dmcore PUBLIC $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS} -std=gnu17>)
target_link_libraries(dmcore PUBLIC dm_vulkan_engine aaudio android log m dl)

# 命令行宿主：main.c 原样 | Command-line host: main.c as is
add_executable(dmhost ${HOST}/main.c)
target_link_libraries(dmhost PRIVATE -Wl,--whole-archive dmcore -Wl,--no-whole-archive)
target_link_options(dmhost PRIVATE -Wl,--no-undefined -Wl,-z,nostart-stop-gc -Wl,--export-dynamic)   # 导出符号：崩溃回溯里 dladdr 能认出宿主函数 | export symbols: dladdr in crash backtraces can name host functions

# App 用的共享库：main.c 以 -Dmain=dm_main 编（同 iOS），JNI 入口 jni_android.c
# Shared library for the App: main.c built with -Dmain=dm_main (as on iOS), JNI entry jni_android.c
# 独立目标只重命名 App 入口，避免影响同目录的命令行目标。
# Keep the renamed entry in a separate target: source properties would also
# rename main in the command-line dmhost target in this directory.
add_library(dm_app_main OBJECT ${HOST}/main.c)
target_link_libraries(dm_app_main PRIVATE dmcore)
target_compile_definitions(dm_app_main PRIVATE main=dm_main)
add_library(dm SHARED $<TARGET_OBJECTS:dm_app_main> ${CMAKE_CURRENT_SOURCE_DIR}/jni_android.c ${CMAKE_CURRENT_SOURCE_DIR}/text_input_jni.c ${CMAKE_CURRENT_SOURCE_DIR}/touch_android_jni.c ${CMAKE_CURRENT_SOURCE_DIR}/overlay_validate.c ${DM_OVERLAY_HEADER})
target_include_directories(dm PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(dm PRIVATE $<$<CONFIG:Debug>:DM_ANDROID_DEBUG_ENV=1>)
target_link_libraries(dm PRIVATE -Wl,--whole-archive dmcore -Wl,--no-whole-archive android)
target_link_options(dm PRIVATE -Wl,--no-undefined -Wl,-z,nostart-stop-gc -Wl,-Bsymbolic)   # 汇编按页寻址宿主全局（adrp + :lo12:）：库内符号就地绑定，不许被抢占 | assembly addresses host globals by page (adrp + :lo12:): symbols bind locally inside the library and cannot be preempted
