cmake_minimum_required(VERSION 3.22)
project(d2r_mobile_android LANGUAGES C ASM)
add_link_options(-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_PLATFORM_LEVEL LESS 29)
  message(FATAL_ERROR "The native guest host requires Android 29+ arm64-v8a")
endif()
add_library(dm_android_gfx OBJECT
  ../gfx/nvn/hle_nvn.c ../gfx/nvn/nvn_entry.S ../gfx/nvn/nvn_thunks.S
  ../gfx/nvn/nvn_tex.c ../gfx/nvn/nvn_dirty.c ../gfx/nvn/nvn_vk.c
  ../gfx/tex/nvn_deswizzle.c ../gfx/tex/nvn_bcdec.c nvn_platform_clock.c
  ../gfx/hle_vi.c ../gfx/hle_nv.c movie_start.c)
target_include_directories(dm_android_gfx PRIVATE ../host . ../gfx/nvn ../gfx/nvn/include)
target_compile_options(dm_android_gfx PRIVATE -Wall -Wextra -Werror -Wno-format -Wno-unused-function -ffixed-x18)
target_compile_definitions(dm_android_gfx PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
set_target_properties(dm_android_gfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dm_android_gfx PUBLIC vulkan android log dl z m)
set(DM_HOST ../host)
set(DM_IOS ../ios/Sources)
add_library(dm_android_core OBJECT
  ${DM_HOST}/loader.c ${DM_HOST}/hle.c ${DM_HOST}/entry.S
  ${DM_HOST}/thread.c ${DM_HOST}/hle_os.c ${DM_HOST}/hle_thread.c
  ${DM_HOST}/hle_svc.c ${DM_HOST}/hle_time.c ${DM_HOST}/hle_guestthread.c
  ${DM_HOST}/hle_sdkinit.c ${DM_HOST}/hle_fs.c thread_slots.c services_sdk.c
  ${DM_HOST}/hle_audio.c codec_opus.c
  ${DM_HOST}/hle_hid.c ${DM_HOST}/hle_touch.c input_backend.c
  ../ios/Sources/dm_touch_controls.c touch_controls_android.c
  ${DM_HOST}/hle_oe.c ${DM_HOST}/hle_account.c ${DM_HOST}/hle_net.c
  ${DM_HOST}/dm_bright.c
  ${DM_HOST}/hle_lifecycle.c ${DM_HOST}/hle_friends.c ${DM_HOST}/hle_misc.c
  ${DM_HOST}/hle_swkbd.c keyboard_android.c
  services_fs.c services_fs_probe.c services_platform.c diagnostics.c svc_host_android.c ui_state_android.c feature_stubs_android.c
  ${DM_HOST}/dm_cursor.c ${DM_HOST}/dm_cursor_item.c ${DM_HOST}/dm_actions.c ${DM_HOST}/dm_world.c ${DM_HOST}/dm_hud.c ${DM_HOST}/dm_uiscreen.c
  ${DM_HOST}/dm_padmode.c commoncrypto_shim.c hud_read_android.c i18n_android.c
  ${DM_IOS}/dm_touch_game_bridge.c ${DM_IOS}/dm_touch_game_state.c ${DM_IOS}/dm_touch_game_identity.c ${DM_IOS}/dm_touch_native_art.c)
# 第三方：libdeflate 只编解压相关源（TACT BLTE zlib 块，svc_host_android.c 用）；代码不动，单独放松告警
# Third party: only the decompression-related sources of libdeflate (TACT BLTE zlib blocks, used by svc_host_android.c); the code is untouched, warnings relaxed in a separate object library
add_library(dm_libdeflate OBJECT
  ../third_party/libdeflate/lib/deflate_decompress.c ../third_party/libdeflate/lib/zlib_decompress.c
  ../third_party/libdeflate/lib/adler32.c ../third_party/libdeflate/lib/utils.c ../third_party/libdeflate/lib/arm/cpu_features.c)
target_compile_options(dm_libdeflate PRIVATE -w)
target_compile_definitions(dm_libdeflate PRIVATE _GNU_SOURCE)
set_target_properties(dm_libdeflate PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dm_android_core PUBLIC ${DM_HOST} . PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_features(dm_android_core PUBLIC c_std_11)
# Upstream's Darwin uint64 format specifiers have the same 64-bit ABI on Bionic.
# Keep that source difference separate from actual width/ABI validation.
target_compile_definitions(dm_android_core PRIVATE DM_ANDROID_SETTINGS_SEED=1)   # 设置缺省 / 语言开关在安卓同样生效（hle_fs.c 的守卫）
target_compile_options(dm_android_core PRIVATE -Wall -Wextra -Wno-format -Wno-unused-function -Wno-missing-field-initializers -Werror -ffixed-x18)
target_compile_definitions(dm_android_core PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
set_target_properties(dm_android_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dm_android_core PUBLIC android log dl m aaudio)
target_link_options(dm_android_core INTERFACE -Wl,-z,nostart-stop-gc)
add_executable(dm_android_host_probe host_probe.c gate_check.S)
target_compile_options(dm_android_host_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18)
target_link_libraries(dm_android_host_probe PRIVATE dm_android_core dm_libdeflate)
target_link_options(dm_android_host_probe PRIVATE -Wl,-z,max-page-size=16384)
add_executable(dm_android_input_probe input_probe.c)
target_compile_options(dm_android_input_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18)
target_link_libraries(dm_android_input_probe PRIVATE dm_android_core dm_libdeflate)
target_link_options(dm_android_input_probe PRIVATE -Wl,-z,max-page-size=16384)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_probe.c")
  add_executable(dm_android_vulkan_probe vulkan_probe.c)
  target_compile_options(dm_android_vulkan_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18)
  target_link_libraries(dm_android_vulkan_probe PRIVATE vulkan android log m)
  target_link_options(dm_android_vulkan_probe PRIVATE -Wl,-z,max-page-size=16384)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/romfs_image.c")
  target_sources(dm_android_core PRIVATE romfs_image.c)
  add_library(d2r_mobile_android SHARED app_jni.c game_resource_probe.c)
  target_include_directories(d2r_mobile_android PRIVATE "${CMAKE_BINARY_DIR}/generated")
  target_compile_options(d2r_mobile_android PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
  target_compile_definitions(d2r_mobile_android PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
  target_link_libraries(d2r_mobile_android PRIVATE dm_android_core dm_android_gfx dm_libdeflate)
  target_link_options(d2r_mobile_android PRIVATE -Wl,-z,max-page-size=16384)
endif()
