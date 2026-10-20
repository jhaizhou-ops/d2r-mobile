cmake_minimum_required(VERSION 3.22)
project(d2r_mobile_android LANGUAGES C ASM)
add_link_options(-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
# ASan 变体（默认关）：只用于抓内存错误。静态链接运行时，免得再往 APK 里放 libclang_rt.asan 和 wrap.sh。
# ASan variant (off by default): memory-error hunting only. The runtime is linked statically so the APK
# does not need libclang_rt.asan plus a wrap.sh.
option(DM_ASAN "Build with AddressSanitizer (static runtime)" OFF)
if(DM_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  # 静态 ASan 运行时：NDK 的 clang 不会自动把它链进 SHARED 库（只对可执行文件生效），
  # 这里显式把归档整段链进来，免得再往 APK 里放 libclang_rt.asan 和 wrap.sh。
  # The NDK clang does not link the static ASan runtime into a SHARED library automatically, so the
  # archive is linked whole here; that avoids shipping libclang_rt.asan plus a wrap.sh in the APK.
  # 共享运行时由 clang 自动加 NEEDED；打包时需要把 libclang_rt.asan-aarch64-android.so 一起放进
  # APK（build_apk.sh 里 DM_PACK_ASAN=1 会做），否则进程解析不到依赖而起不来。
  # The shared runtime is added as a NEEDED entry by clang; build_apk.sh packs
  # libclang_rt.asan-aarch64-android.so into the APK when DM_PACK_ASAN=1, otherwise the process
  # cannot resolve the dependency and fails to start.
  add_link_options(-fsanitize=address)
endif()
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_PLATFORM_LEVEL LESS 29)
  message(FATAL_ERROR "The native guest host requires Android 29+ arm64-v8a")
endif()
add_library(dm_android_gfx OBJECT
  ../gfx/nvn/hle_nvn.c ../gfx/nvn/nvn_entry.S ../gfx/nvn/nvn_thunks.S
  ../gfx/nvn/nvn_tex.c ../gfx/nvn/nvn_dirty.c
  ../gfx/tex/nvn_deswizzle.c ../gfx/tex/nvn_bcdec.c nvn_platform_clock.c
  ../gfx/hle_vi.c ../gfx/hle_nv.c movie_start.c
# Vulkan 回放后端从通用那一行挪出来，单独一行：它只属于 Android（对门是 Metal 的 nvn_mtl.m，两边同一组 mtl_* 符号）。
# nvn_vk.c 自己还有一层 #ifdef __ANDROID__ 守卫（见该文件头部），为的是 Mac / iOS / tvOS 那些按 ../gfx/nvn/*.c 通配收源的
# 构建不被它拖死；那层守卫有个隐患：万一 NDK 工具链没定义 __ANDROID__，这里会静默编出空对象，报错要拖到链接期才以
# 一堆 undefined mtl_* 出现。所以下面给这个目标定义 DM_EXPECT_VULKAN_BACKEND：nvn_vk.c 走到空翻译单元那条支路时会 #error 当场停下。
# The Vulkan replay backend is moved off the shared line onto its own line: it belongs to Android only (its counterpart is the
# Metal nvn_mtl.m; the two implement the same mtl_* symbols). nvn_vk.c also carries an #ifdef __ANDROID__ guard of its own (see
# its file header) so that the builds collecting sources by the ../gfx/nvn/*.c wildcard (Mac / iOS / tvOS) are not dragged down
# by it. That guard has one hazard: if the NDK toolchain did not define __ANDROID__, this would silently build an empty object
# and the failure would only surface at link time as a pile of undefined mtl_*. So the target defines DM_EXPECT_VULKAN_BACKEND
# below: nvn_vk.c hits #error and stops right there if it ever reaches the empty-translation-unit branch.
  ../gfx/nvn/nvn_vk.c)
target_include_directories(dm_android_gfx PRIVATE ../host . ../gfx/nvn ../gfx/nvn/include)
target_compile_options(dm_android_gfx PRIVATE -Wall -Wextra -Werror -Wno-format -Wno-unused-function -ffixed-x18)
target_compile_definitions(dm_android_gfx PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_EXPECT_VULKAN_BACKEND=1)
set_target_properties(dm_android_gfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dm_android_gfx PUBLIC vulkan android log dl z m)
set(DM_HOST ../host)
set(DM_IOS ../ios/Sources)
# 技能索引那份 .c：不用 iOS 源码树里那份，用 gen_overlay.sh 复制到 Android 构建树的那份（那边还换掉了相对 include）。
# dm/host 已在下面的 include 路径里，副本里的 #include "dm_input.h" 照样找得到；iOS 那边一个字没动。
# The .c skill index: not the one in the iOS source tree but the copy gen_overlay.sh staged into the Android build tree (which also rewrote its
# relative include). dm/host is in the include path below, so the copy's #include "dm_input.h" resolves; nothing on the iOS side changed.
set(DM_NATIVE_ART_C ${CMAKE_BINARY_DIR}/gen-assets/native_art/dm_touch_native_art.c)
if(DEFINED ENV{DM_GEN_ASSETS})   # 与 gen_paths.gen_dir() 同一个约定：环境变量优先，缺省按构建目录。设了别的根就跟着走，别两边错位
  set(DM_NATIVE_ART_C $ENV{DM_GEN_ASSETS}/native_art/dm_touch_native_art.c)   # same convention as gen_paths.gen_dir(): the environment variable wins, otherwise the build directory; follow a different root rather than let the two drift apart
endif()
if(NOT EXISTS ${DM_NATIVE_ART_C})
  message(FATAL_ERROR "缺 ${DM_NATIVE_ART_C}\n先跑 dm/android/tools/gen_overlay.sh 生成资源与那份 .c 副本（它要从你的游戏文件生成，仓库里没有）")
endif()
# 服务层接管 hle_*.c 按通配纳入（同 dm/host/Makefile 的做法）：新增文件不再漏。此前这里是逐个手写的，
# hle_charname.c 就因此静默漏编过——中文角色名一律进不去（真机：输了中文，名字框里什么也不剩），
# 而它在文档里好好地列着，谁都以为编了。通配就是为了不再有这种事。
# The service-layer hle_*.c files are included by wildcard (same as dm/host/Makefile does): a new file can no longer be missed. This list used
# to be written out one by one, and hle_charname.c was silently left out of it -- no Chinese character name could get through (real device: type one
# and nothing is left in the name box), while the docs listed it as if it were compiled. A wildcard exists so that stops happening.
get_filename_component(DM_HOST_ABS ${DM_HOST} ABSOLUTE)   # 通配与排除表都用绝对路径：相对路径下 GLOB 的返回形式会变，REMOVE_ITEM 就对不上了
                                                           # The glob and the exclusion table both use absolute paths: with a relative pattern the form of GLOB's results changes and REMOVE_ITEM no longer matches
file(GLOB DM_HLE_ALL CONFIGURE_DEPENDS ${DM_HOST_ABS}/hle_*.c)
# 不编的逐条写明原因。新增一个 hle_*.c 若不在这张表里，就该被下面的断言要求表态，而不是悄悄漏掉或悄悄进来。
# Each one not built, with its reason. A new hle_*.c not listed here is caught by the assertion below and has to take a side, instead of being
# silently missed or silently pulled in.
set(DM_HLE_SKIP
  hle_codec        # 硬件 Opus 解码：Android 侧 codec_opus.c 已经接管同一个 HLE（nn::codec::HardwareOpusDecoder 构造），两个都编撞符号
  hle_count        # Mac 诊断（DM_HLECOUNT 的 HLE 覆盖审计），依赖 OSCacheControl.h
  hle_fdstat       # 诊断 DM_FD_LOG 的文件描述符统计，用到 getdtablesize（Apple 独有）
  hle_granny       # Granny 段读取 svc 0x83/0x84：Android 侧 svc_host_android.c 里有等价实现（SVC_HOST_GRANNY_A/B），两个都编撞符号
  hle_movie        # 过场/背景视频空播：Android 侧 movie_start.c 已经接管 movie::* 的 HLE
  hle_private_aim  # 私有技能朝向适配：Android 侧 svc_host_android.c 里有等价实现（SVC_HOST_PRIVATE_SKILL_*）
  hle_table        # libc 直通表靠 iOS 出包的 swhle 段挂载，CMake 这条链上没有那个段，编进来也不会生效
  hle_tact)        # TACT 的 BLTE 解压走 libdeflate：Android 侧 svc_host_android.c 里有等价实现（SVC_HOST_TACT_ZLIB）
foreach(_s IN LISTS DM_HLE_SKIP)
  list(REMOVE_ITEM DM_HLE_ALL ${DM_HOST_ABS}/${_s}.c)   # 把表里那些从通配结果里摘掉 | drop the listed ones from the glob result
endforeach()
# The libc passthrough table relies on the swhle segment of the iOS packaging to install itself; this chain has no such segment, so building it
# would have no effect.
# 断言只查排除表这一侧：表里每一项都得是真存在的文件。文件改名/删除后若不更新表，会留下一条假的排除项，
# 那个文件就此静默不编——正是 hle_charname.c 当年那种坑，所以要在 configure 时挡住。
# 通配本身已经保证了另一半：不在表里的 hle_*.c 一律编进来，新增文件不会漏掉（该不编就往表里加一条，编不过会当场报错，不会静默）。
# The assertion only covers the exclusion side: every entry must be a file that exists. After a rename or removal, an unupdated table leaves a
# phantom exclusion and that file is silently not built -- exactly the trap hle_charname.c fell into, so it is blocked at configure time.
# The wildcard already covers the other half: anything not in the table is built, so a new file cannot be missed (if it should not be built, add
# a line to the table -- and if it does not compile, that fails loudly rather than silently).
foreach(_s IN LISTS DM_HLE_SKIP)
  get_filename_component(_p ${DM_HOST}/${_s}.c ABSOLUTE)   # EXISTS 吃相对路径不可靠，先取绝对 | EXISTS with a relative path is unreliable, take it absolute first
  if(NOT EXISTS ${_p})
    message(FATAL_ERROR "排除表里的 ${_s}.c 不存在（改名或删了？）：删掉 DM_HLE_SKIP 里那一行，或改成新名字。\n"
                        "Excluded ${_s}.c does not exist (renamed or removed?): drop that line from DM_HLE_SKIP or update it to the new name.")
  endif()
endforeach()
if(NOT DM_HLE_ALL)
  message(FATAL_ERROR "通配没拿到任何 hle_*.c，路径不对？${DM_HOST}")
endif()

add_library(dm_android_core OBJECT
  ${DM_HOST}/loader.c ${DM_HOST}/hle.c ${DM_HOST}/entry.S
  ${DM_HOST}/thread.c
  ${DM_HLE_ALL}
  ${DM_HOST}/hle_sdkinit.c thread_slots.c services_sdk.c
  ${DM_HOST}/hle_audio.c codec_opus.c
  input_backend.c
  ../ios/Sources/dm_touch_controls.c touch_controls_android.c
  ${DM_HOST}/hle_oe.c
  ${DM_HOST}/dm_bright.c
  keyboard_android.c
  services_fs.c services_fs_probe.c services_platform.c diagnostics.c svc_host_android.c ui_state_android.c feature_stubs_android.c
  ${DM_HOST}/dm_cursor.c ${DM_HOST}/dm_cursor_item.c ${DM_HOST}/dm_actions.c ${DM_HOST}/dm_world.c ${DM_HOST}/dm_hud.c ${DM_HOST}/dm_uiscreen.c
  ${DM_HOST}/dm_padmode.c ${DM_HOST}/dm_i18n.c ${DM_HOST}/dm_freetp.c ${DM_HOST}/dm_potpick.c ${DM_HOST}/dm_beltauto.c ${DM_HOST}/dm_autobuff.c ${DM_HOST}/dm_lootfilter.c ${DM_HOST}/dm_custom.c ${DM_HOST}/dm_kills.c ${DM_HOST}/dm_elysium.c ${DM_HOST}/dm_rift.c ${DM_HOST}/dm_bossrush.c ${DM_HOST}/dm_pages.c ${DM_HOST}/dm_vault.c ${DM_HOST}/dm_vault_ui.c ${DM_HOST}/dm_runeword.c ${DM_HOST}/dm_pouch.c ${DM_HOST}/dm_textbox.c ${DM_HOST}/dm_update.c ${DM_HOST}/dm_townwp.c ${DM_HOST}/dm_textbox_core.c ${DM_HOST}/dm_rwlv_core.c ${DM_HOST}/dm_rift_ui.c ${DM_HOST}/dm_turnsmooth.c commoncrypto_shim.c hud_read_android.c diag_android.c i18n_android.c
  ${DM_IOS}/dm_touch_game_bridge.c ${DM_IOS}/dm_touch_game_state.c ${DM_IOS}/dm_touch_game_identity.c ${DM_NATIVE_ART_C})
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
target_compile_options(dm_android_core PRIVATE -Wall -Wextra -Wno-format -Wno-unused-function -Wno-missing-field-initializers -Wno-unused-but-set-variable -Wno-unused-parameter -Werror -ffixed-x18)
# Apple 专有写法（clock_gettime_nsec_np / st_mtimespec）在安卓的等价垫片，强制编进宿主 .c
# Apple-only spellings (clock_gettime_nsec_np / st_mtimespec): the Android equivalent shim, force-included into the host sources
target_compile_options(dm_android_core PRIVATE $<$<COMPILE_LANGUAGE:C>:-include;${CMAKE_CURRENT_SOURCE_DIR}/apple_time_android.h>)   # 只对 C 生效（汇编不能带 -include）
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
