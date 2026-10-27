DM_ROOT ?= $(HOME)/dm-work
# 最低系统版本：图形后端（gfx/nvn/nvn_mtl.m）整个建在 Metal 4 上，macOS 26 才有；不显式写的话编译器取本机系统的版本。
# 打包脚本（tools/mac_app.sh）用 print-macos-min 取同一个数写进 Info.plist，并核程序本体里的数与它相等
# Minimum system version: the graphics backend (gfx/nvn/nvn_mtl.m) is built entirely on Metal 4, which exists from macOS 26; without stating it explicitly the compiler takes the version of the local system.
# The packaging script (tools/mac_app.sh) takes the same number through print-macos-min, writes it into Info.plist, and checks that the number in the executable equals it
MACOS_MIN ?= 26.0
CFLAGS=-O1 -g -Wall -Wno-unused-function -mmacosx-version-min=$(MACOS_MIN)
MOVIE_FFMPEG ?= 0
MOVIE_PREFIX ?=
ifeq ($(MOVIE_FFMPEG),1)
ifneq ($(MOVIE_PREFIX),)
# iOS 交叉编译使用已经编好的静态库，不依赖宿主 pkg-config（脚本见 dm/ios/tools/build_ffmpeg_ios.sh）。
CFLAGS += -DDM_MOVIE_FFMPEG=1 -I$(MOVIE_PREFIX)/include
MOVIE_LIBS = $(MOVIE_PREFIX)/lib/libavformat.a $(MOVIE_PREFIX)/lib/libavcodec.a $(MOVIE_PREFIX)/lib/libswscale.a $(MOVIE_PREFIX)/lib/libavutil.a
else
MOVIE_PACKAGES = libavformat libavcodec libavutil libswscale
ifeq ($(shell pkg-config --exists $(MOVIE_PACKAGES) && echo yes),)
$(error MOVIE_FFMPEG=1 requires pkg-config and FFmpeg development libraries, or MOVIE_PREFIX pointing to a static FFmpeg build)
endif
CFLAGS += -DDM_MOVIE_FFMPEG=1 $(shell pkg-config --cflags $(MOVIE_PACKAGES))
MOVIE_LIBS = $(shell pkg-config --libs $(MOVIE_PACKAGES))
endif
endif
# 服务层接管 hle_*.c 按通配纳入：新增文件不改这一行（免合并冲突）
# 一行一个、按字母序（先本目录的，再 ../ 开头的，最后是通配那一项）：加源文件时插到自己的位置，别的分支加的在别的行，变基不再整行冲突
# The service-layer hle_*.c files are included by wildcard: adding a file does not change this line (no merge conflicts)
# One per line, in alphabetical order (first those in this directory, then those starting with ../, the wildcard entry last): insert a new source file at its own place; files added on other branches are on other lines, so rebasing no longer conflicts on the whole line
SRC=\
	app_mac.m \
	crash.c \
	diag.c \
	entry.S \
	front_mac.m \
	hle.c \
	loader.c \
	main.c \
	memstat.m \
	romfs_validate.c \
	dm_actions.c \
	dm_autobuff.c \
	dm_beltauto.c \
	dm_bossrush.c \
	dm_bright.c \
	dm_cam.c \
	dm_cloudsync.c \
	dm_cursor.c \
	dm_cursor_item.c \
	dm_custom.c \
	dm_dropfilter.c \
	dm_drophaptic.c \
	dm_elysium.c \
	dm_freetp.c \
	dm_hud.c \
	dm_i18n.c \
	dm_kills.c \
	dm_lootfilter.c \
	dm_movie_decode.c \
	dm_namegen.c \
	dm_padmode.c \
	dm_pages.c \
	dm_potpick.c \
	dm_pouch.c \
	dm_cloudmerge.c \
	dm_rift.c \
	dm_rift_ui.c \
	dm_runeword.c \
	dm_rwlv_core.c \
	dm_shoprefresh.c \
	dm_textbox.c \
	dm_textbox_core.c \
	dm_textinput.c \
	dm_townwp.c \
	dm_turnsmooth.c \
	dm_uiscreen.c \
	dm_update.c \
	dm_vault.c \
	dm_vault_ui.c \
	dm_world.c \
	dm_prof.c \
	thread.c \
	touch_mac.c \
	../ios/Sources/cloud_ios.m \
	../ios/Sources/dm_gamepad.m \
	../ios/Sources/dm_touch_controls.c \
	$(sort $(wildcard hle_*.c))
# NVN 空设备 + nn::vi（dm/gfx/）
# 按通配纳入 dm/gfx 与 dm/gfx/nvn 下的源（tests/ metal/ 不进宿主）：新增文件不改这一行
# NVN null device + nn::vi (dm/gfx/)
# Sources under dm/gfx and dm/gfx/nvn are included by wildcard (tests/ and metal/ do not go into the host): adding a file does not touch this line
GFX=$(sort $(wildcard ../gfx/*.c ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m))
GFX_H=$(sort $(wildcard ../gfx/nvn/*.h ../gfx/nvn/include/*.h ../gfx/tex/*.h))
# 第三方：libdeflate 只编解压相关源（TACT BLTE zlib 块，hle_tact.c）
# Third party: only the decompression-related sources of libdeflate are built (TACT BLTE zlib blocks, hle_tact.c)
LIBDEFLATE=../third_party/libdeflate/lib/deflate_decompress.c ../third_party/libdeflate/lib/zlib_decompress.c ../third_party/libdeflate/lib/adler32.c ../third_party/libdeflate/lib/utils.c ../third_party/libdeflate/lib/arm/cpu_features.c
.DEFAULT_GOAL := dmhost   # 下面先写了表头生成规则，缺省目标要明说（否则 make 只生成表头、不编宿主） | The header generation rules are written first below, so the default goal must be stated explicitly (otherwise make only generates the headers and does not build the host)
print-macos-min:
	@echo $(MACOS_MIN)
# 旧接口版（图形后端不用 Metal 4，见 gfx/nvn/nvn_mtl.m 的 DM_NVN_M3）：最低系统单独一个数；包名另起，存档与日志目录跟着包名走、不与正式包共用
# Older-API build (graphics backend without Metal 4, see DM_NVN_M3 in gfx/nvn/nvn_mtl.m): its minimum system is a separate number; the package gets its own name, and the save and log directory follows the package name instead of being shared with the regular package
MACOS_MIN_M3 ?= 15.0
CFLAGS_M3=$(subst -mmacosx-version-min=$(MACOS_MIN),-mmacosx-version-min=$(MACOS_MIN_M3),$(CFLAGS)) -DDM_NVN_M3=1 -DAPP_NAME='"D2R Mobile M3"'
print-macos-min-m3:
	@echo $(MACOS_MIN_M3)
# 由原版表生成的头文件：不进仓库，构建时从玩家自己的游戏文件（Switch 版 3.3.94012 的 Update/RomFS）生成；路径用环境变量 DM_ROMFS_SRC 指定
# Headers generated from the original game's tables: not checked into the repository, generated at build time from the player's own game files (Update/RomFS of Switch version 3.3.94012); the path is given by the environment variable DM_ROMFS_SRC
ROMFS?=$(or $(DM_ROMFS_SRC),$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS)
GENH=dm_vault_tab.h dm_hud_exp.h dm_bright_levels.inc
NEED_ROMFS=@test -f "$(ROMFS)/data/build_manifest.psv" || { echo "🔴 没找到游戏文件：$(ROMFS)/data/build_manifest.psv。把 Switch 版 3.3.94012 的 Update/RomFS 放到这里，或用环境变量 DM_ROMFS_SRC 指过去（见 dm/README.md「从你的游戏文件生成资源」）"; exit 1; }
dm_vault_tab.h: ../ios/tools/gen_vault_table.py
	$(NEED_ROMFS)
	python3 ../ios/tools/gen_vault_table.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
dm_hud_exp.h: ../ios/tools/gen_hud_exp.py ../ios/tools/romfs_art.py
	$(NEED_ROMFS)
	python3 ../ios/tools/gen_hud_exp.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
dm_bright_levels.inc: ../tools/gen_bright_levels.py ../ios/tools/touch_layout.py
	$(NEED_ROMFS)
	python3 ../tools/gen_bright_levels.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
dmhost: $(SRC) $(GFX) $(LIBDEFLATE) dm.h hle.h dm_input.h front.h touch_mac.h dm_cursor.h dm_movie_decode.h dm_i18n_tab.h app_mac_overlay.h ../ios/Sources/dm_touch_controls.[ch] $(GFX_H) $(GENH)
	clang $(CFLAGS) -I. -I../gfx/nvn/include -fobjc-arc -o $@ $(SRC) $(GFX) $(LIBDEFLATE) $(MOVIE_LIBS) -framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox -framework CoreAudio -framework ImageIO -framework UniformTypeIdentifiers -framework GameController -framework Network
# 旧接口版宿主（图形后端不用 Metal 4，目标 macOS 15.0）：同一棵树、同一份源码，只多一个编译开关；不在缺省目标里，要用时 make dmhost_m3
# Older-API host (graphics backend without Metal 4, target macOS 15.0): the same tree and the same source, with just one more compile switch; not in the default targets, run make dmhost_m3 when needed
dmhost_m3: $(SRC) $(GFX) $(LIBDEFLATE) dm.h hle.h dm_input.h front.h touch_mac.h dm_cursor.h dm_movie_decode.h dm_i18n_tab.h app_mac_overlay.h ../ios/Sources/dm_touch_controls.[ch] $(GFX_H) $(GENH)
	clang $(CFLAGS_M3) -I. -I../gfx/nvn/include -fobjc-arc -o $@ $(SRC) $(GFX) $(LIBDEFLATE) $(MOVIE_LIBS) -framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox -framework CoreAudio -framework ImageIO -framework UniformTypeIdentifiers -framework GameController -framework Network
