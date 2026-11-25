# 鸿蒙宿主构建（OHOS SDK，arm64-v8a）：与安卓同一份公共宿主 dm/host + Vulkan 引擎，源文件清单同样从 dm/host/Makefile 取；
# 苹果接口薄实现复用 dm/android/compat（鸿蒙同为 Linux/musl，见 dm_android_compat.h 的 __OHOS__ 分支）。平台层在本目录。
# HarmonyOS host build (OHOS SDK, arm64-v8a): the same shared host dm/host + Vulkan engine as Android, with the source list taken
# from dm/host/Makefile; the thin Apple-API shims reuse dm/android/compat (HarmonyOS is Linux/musl too, see the __OHOS__ branch of
# dm_android_compat.h). The platform layer lives in this directory.
# 必需：-DDM_GEN_DIR=<由游戏文件生成的宿主表头目录>（不入库） | Required: -DDM_GEN_DIR=<host table headers generated from game files> (not committed)
cmake_minimum_required(VERSION 3.22)
project(dm_ohos C CXX ASM)
set(DM ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(HOST ${DM}/host)
set(AND ${DM}/android)
# 原生 ELF TLS（默认 emutls：每次 thread_local 访问都走 __emutls_get_address → pthread_getspecific，翻译线程 hiperf 见 ~10%）；
# emutls 本就经 musl 读 TPIDR_EL0，换原生 TLS 不新增依赖 | native ELF TLS (the default emutls makes every thread_local access call
# __emutls_get_address → pthread_getspecific, ~10% of the translation thread in hiperf); emutls already reads TPIDR_EL0 via musl
add_compile_options(-fno-emulated-tls)
# 本机路径不进安装包：源码、构建与生成目录都映射成相对名（同安卓），由 check_release_clean.py 检查 | keep machine paths out of the package: the source, build and
# generated directories map to relative names (as Android); checked by check_release_clean.py
get_filename_component(DM_ROOT_ABS ${DM}/.. ABSOLUTE)
add_compile_options(-ffile-prefix-map=${DM_ROOT_ABS}=. -ffile-prefix-map=${CMAKE_BINARY_DIR}=build)
if(DM_GEN_DIR)
  get_filename_component(DM_GEN_ABS ${DM_GEN_DIR} ABSOLUTE)
  add_compile_options(-ffile-prefix-map=${DM_GEN_ABS}=gen)
endif()
add_subdirectory(${DM}/gfx/vulkan ${CMAKE_CURRENT_BINARY_DIR}/vulkan)
# 调试构建可显式运行同一等待 helper 的并发回归；默认不运行，发布包不编入。
# Debug-only concurrency regression for the production wait helper; opt-in at runtime, omitted from release.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_sources(dm_vulkan_engine PRIVATE ${DM}/gfx/tests/t_completion_wait.cpp)
  target_compile_definitions(dm_vulkan_engine PRIVATE DM_COMPLETION_WAIT_SELFTEST=1)
endif()
# 引擎源码不调安卓日志（日志走回调）；鸿蒙没有 liblog，去掉这一项 | The engine never calls Android logging (logs go through a callback); HarmonyOS has no liblog
get_target_property(_eng_libs dm_vulkan_engine INTERFACE_LINK_LIBRARIES)
list(REMOVE_ITEM _eng_libs log)
set_target_properties(dm_vulkan_engine PROPERTIES INTERFACE_LINK_LIBRARIES "${_eng_libs}")
if(NOT DM_GEN_DIR OR NOT EXISTS ${DM_GEN_DIR}/dm_vault_tab.h)
  message(FATAL_ERROR "要 -DDM_GEN_DIR=<宿主表头目录> | need -DDM_GEN_DIR")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${AND}/tools/host_sources.py ${HOST}/Makefile ${CMAKE_CURRENT_BINARY_DIR}/host_sources.cmake RESULT_VARIABLE r)
if(NOT r EQUAL 0)
  message(FATAL_ERROR "Cannot parse host sources")
endif()
include(${CMAKE_CURRENT_BINARY_DIR}/host_sources.cmake)
list(REMOVE_DUPLICATES HOST_SRCS)
file(GLOB HLE_SRCS CONFIGURE_DEPENDS ${HOST}/hle_*.c)
file(GLOB GFX_SRCS CONFIGURE_DEPENDS ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)
set(ASM_SRCS)
foreach(s ${HOST}/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
  get_filename_component(n ${s} NAME)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/asm/${n})
  add_custom_command(OUTPUT ${out} COMMAND ${Python3_EXECUTABLE} ${AND}/tools/asm_elf.py ${s} ${out} DEPENDS ${s} ${AND}/tools/asm_elf.py VERBATIM)
  list(APPEND ASM_SRCS ${out})
endforeach()
set(COMPAT_SRCS ${AND}/compat/clonefile.c ${AND}/compat/digest.c ${AND}/compat/mach_thread.c ${AND}/compat/mach_symbols.c ${DM}/gfx/vulkan/nvn_vk.c)
# 触控与 iOS 共用的公共 C（技能图标索引由玩家资源在仓外生成，同安卓） | Touch C shared with iOS (the skill icon index is generated outside the repo from the player's files, as Android)
if(NOT EXISTS ${DM_GEN_DIR}/dm_touch_native_art.c)
  message(FATAL_ERROR "Generate native_art/generate.py <romfs> --output-dir <DM_GEN_DIR> first")
endif()
list(APPEND COMPAT_SRCS ${DM}/ios/Sources/dm_touch_game_state.c ${DM}/ios/Sources/dm_touch_game_identity.c ${DM}/ios/Sources/dm_touch_game_bridge.c ${DM_GEN_DIR}/dm_touch_native_art.c)
add_library(dmcore STATIC ${HOST_SRCS} ${HLE_SRCS} ${GFX_SRCS} ${LIBDEFLATE_SRCS} ${ASM_SRCS} ${COMPAT_SRCS})
# The private AppGallery package carries pre-generated modules as signed ELF libraries.
# Empty public shells keep their existing import/debug-signing workflow.
option(DM_OHOS_SIGNED_GUEST "Load module code from HAP-signed native libraries" OFF)
if(DM_OHOS_SIGNED_GUEST)
  target_sources(dmcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/signed_guest_image.c)
  target_compile_definitions(dmcore PUBLIC DM_OHOS_SIGNED_GUEST=1)
endif()
target_include_directories(dmcore PUBLIC ${HOST} ${DM}/gfx/nvn/include ${AND}/compat/include ${DM_GEN_DIR} ${DM}/ios/Sources)
# 宿主 / 平台层用 -O2（原 -O1：游戏线程每次 NVN 调用的命令录制都跑在这里；-fno-strict-aliasing 保留）| host / platform at -O2 (was -O1: every NVN call of the game thread records here; -fno-strict-aliasing kept)
set(DM_OPT_LEVEL -O2)
set(DM_H12_BUILD "$ENV{DM_H12_BUILD}")
if(DM_H12_BUILD AND NOT DM_H12_BUILD STREQUAL "baseline")
  if(NOT DM_H12_BUILD MATCHES "^(thin|generate|use)$")
    message(FATAL_ERROR "Unknown DM_H12_BUILD: ${DM_H12_BUILD}")
  endif()
  set(DM_OPT_LEVEL -O3)
endif()
set(DM_CFLAGS ${DM_OPT_LEVEL} -g -D_GNU_SOURCE -Wall -Wno-unused-function -fno-strict-aliasing -include ${AND}/compat/include/dm_android_compat.h)
target_compile_options(dmcore PUBLIC $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS} -std=gnu17>)

# ---- 平台层：dm/android 里平台无关的文件原样复用，安卓头文件由 android_shim 替身接到鸿蒙 ----
# ---- Platform layer: platform-neutral files in dm/android are reused unchanged; android_shim maps their Android headers to HarmonyOS ----
set(OHOS_REUSED ${AND}/vulkan_present.cpp ${AND}/overlay_validate.c ${CMAKE_CURRENT_SOURCE_DIR}/front_ohos.c ${CMAKE_CURRENT_SOURCE_DIR}/touch_ohos.c ${CMAKE_CURRENT_SOURCE_DIR}/gameperf_ohos.c)
add_library(dmplatform STATIC ${OHOS_REUSED} ${CMAKE_CURRENT_SOURCE_DIR}/gamepad_ohos.c ${CMAKE_CURRENT_SOURCE_DIR}/haptics_ohos.c ${CMAKE_CURRENT_SOURCE_DIR}/frame_rate_ohos.cpp)
target_include_directories(dmplatform BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/android_shim)
target_include_directories(dmplatform PRIVATE ${AND})
target_link_libraries(dmplatform PUBLIC dmcore dm_vulkan_engine)
target_compile_options(dmplatform PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS} -std=gnu17> $<$<COMPILE_LANGUAGE:CXX>:${DM_OPT_LEVEL} -g -D_GNU_SOURCE>)

# ---- overlay 合约头（与安卓同一工具生成） | Overlay contract header (same tool as Android) ----
set(DM_OVERLAY_TOOL ${DM}/ios/tools/overlay_contract.py)
get_filename_component(DM_REPO_ROOT ${DM}/.. ABSOLUTE)
set(DM_OVERLAY_HEADER ${CMAKE_CURRENT_BINARY_DIR}/dm_overlay_contract.h)
# Recompute at configure time as well: switching branches can leave an output
# newer than every dependency even though its content belongs to another tree.
execute_process(COMMAND ${Python3_EXECUTABLE} ${DM_OVERLAY_TOOL} --inputs
  OUTPUT_VARIABLE DM_OVERLAY_INPUT_LIST RESULT_VARIABLE DM_OVERLAY_INPUT_RESULT
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT DM_OVERLAY_INPUT_RESULT EQUAL 0)
  message(FATAL_ERROR "Cannot enumerate overlay contract inputs")
endif()
string(REPLACE "\r" "" DM_OVERLAY_INPUT_LIST "${DM_OVERLAY_INPUT_LIST}")
string(REPLACE "\n" ";" DM_OVERLAY_INPUT_LIST "${DM_OVERLAY_INPUT_LIST}")
set(DM_OVERLAY_DEPENDS)
foreach(input IN LISTS DM_OVERLAY_INPUT_LIST)
  list(APPEND DM_OVERLAY_DEPENDS "${DM_REPO_ROOT}/${input}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DM_OVERLAY_DEPENDS})
execute_process(COMMAND ${Python3_EXECUTABLE} ${DM_OVERLAY_TOOL}
  --repo-root ${DM_REPO_ROOT} --header ${DM_OVERLAY_HEADER}
  RESULT_VARIABLE DM_OVERLAY_HEADER_RESULT)
if(NOT DM_OVERLAY_HEADER_RESULT EQUAL 0)
  message(FATAL_ERROR "Cannot generate overlay contract header")
endif()
add_custom_command(OUTPUT ${DM_OVERLAY_HEADER}
  COMMAND ${Python3_EXECUTABLE} ${DM_OVERLAY_TOOL} --repo-root ${DM_REPO_ROOT} --header ${DM_OVERLAY_HEADER}
  DEPENDS ${DM_OVERLAY_DEPENDS} VERBATIM)

# ---- App 共享库 libdm.so：main.c 以 -Dmain=dm_main 编（同 iOS / 安卓），NAPI 入口 napi_ohos.c ----
# ---- App shared library libdm.so: main.c built with -Dmain=dm_main (as iOS / Android), NAPI entry napi_ohos.c ----
add_library(dm_app_main OBJECT ${HOST}/main.c)
target_link_libraries(dm_app_main PRIVATE dmcore)
target_compile_definitions(dm_app_main PRIVATE main=dm_main)
add_library(dm SHARED $<TARGET_OBJECTS:dm_app_main> ${CMAKE_CURRENT_SOURCE_DIR}/napi_ohos.c ${DM_OVERLAY_HEADER})
target_include_directories(dm BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/android_shim)
target_include_directories(dm PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${AND})
target_compile_options(dm PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS} -std=gnu17>)
# 导入包配套：pack_info.txt 的 PRODUCT 要等于 dm/PRODUCT 第一行（安卓编进 BuildConfig.DM_PRODUCT） | import pack match: PRODUCT in pack_info.txt must equal the
# first line of dm/PRODUCT (Android compiles it into BuildConfig.DM_PRODUCT)
file(STRINGS ${DM}/PRODUCT DM_PRODUCT_LINE LIMIT_COUNT 1)
target_compile_definitions(dm PRIVATE DM_PRODUCT="${DM_PRODUCT_LINE}")
target_link_libraries(dm PRIVATE -Wl,--whole-archive dmplatform dmcore -Wl,--no-whole-archive dm_vulkan_engine
  ace_napi.z ace_ndk.z hilog_ndk.z hitrace_ndk.z native_window native_display_manager ohaudio qos ohgame_controller.z ohvibrator.z vulkan m dl)
target_link_options(dm PRIVATE -Wl,--no-undefined -Wl,-z,nostart-stop-gc -Wl,-Bsymbolic)

# Diagnostic packages only. Keep the distribution configuration and its hot
# paths unchanged when the build environment contains no H-12 options.
if("$ENV{DM_H12_MEASURE}" STREQUAL "1")
  target_sources(dmcore PRIVATE h12_measure.c)
  # The fixed reference must keep the same loop under every compiler A/B.
  # Per-source options are emitted after the experimental target options.
  set_source_files_properties(h12_measure.c PROPERTIES COMPILE_OPTIONS
    "-O2;-mcpu=generic;-fno-lto;-fno-profile-generate;-fno-profile-use")
  target_compile_definitions(dmcore PRIVATE DM_H12_MEASURE=1)
  target_compile_definitions(dm_vulkan_engine PRIVATE DM_H12_MEASURE=1)
endif()
if(DM_OPT_LEVEL STREQUAL "-O3")
  set(h12_compile -O3 -flto=thin)
  set(h12_link -flto=thin)
  if(NOT "$ENV{DM_H12_CPU}" STREQUAL "")
    list(APPEND h12_compile "-mcpu=$ENV{DM_H12_CPU}")
  endif()
  if(DM_H12_BUILD STREQUAL "generate")
    list(APPEND h12_compile -fprofile-generate -fprofile-update=atomic)
    list(APPEND h12_link -fprofile-generate)
    target_sources(dm PRIVATE h12_pgo.c)
    set_source_files_properties(h12_pgo.c PROPERTIES COMPILE_OPTIONS "-fno-profile-generate")
  elseif(DM_H12_BUILD STREQUAL "use")
    if(NOT EXISTS "$ENV{DM_H12_PROFDATA}")
      message(FATAL_ERROR "DM_H12_PROFDATA must point to the merged training profile")
    endif()
    list(APPEND h12_compile "-fprofile-use=$ENV{DM_H12_PROFDATA}" -Werror=profile-instr-out-of-date)
    list(APPEND h12_link "-fprofile-use=$ENV{DM_H12_PROFDATA}")
  endif()
  foreach(target dm_vulkan_engine dmcore dmplatform dm_app_main dm)
    foreach(flag IN LISTS h12_compile)
      target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${flag}>")
    endforeach()
  endforeach()
  target_link_options(dm PRIVATE ${h12_link})
endif()
