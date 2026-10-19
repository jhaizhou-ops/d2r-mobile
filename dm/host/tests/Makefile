DM_ROOT ?= $(HOME)/dm-work
# dm/host 单测一条命令：make -C dm/host/tests run（秒级；产物放 build/ 不进仓）
# dm/host unit tests in one command: make -C dm/host/tests run (takes seconds; build output goes to build/ and is not committed)
PY ?= $(DM_ROOT)/build/cache/dm/venv/bin/python
EXEFS ?= $(DM_ROOT)/data/switch/3.3.94012/Update/ExeFS
B = build
# 依赖一行一个、按字母序（同 ../Makefile 的 SRC：免得加一个测试就整行冲突）；下面各条命令的先后不变
# Dependencies one per line in alphabetical order (same as SRC in ../Makefile: so that adding one test does not conflict on the whole line); the order of the commands below is unchanged
run: \
	$(B)/main.orig.bin \
	$(B)/subsdk0.orig.bin \
	$(B)/t_altstack \
	$(B)/t_audio \
	$(B)/t_audio_spsc \
	$(B)/t_cloudsync \
	$(B)/t_condvar \
	$(B)/t_condvar_old \
	$(B)/t_doorspot \
	$(B)/t_fs \
	$(B)/t_fs_case \
	$(B)/t_hid_receipt \
	$(B)/t_hid_script \
	$(B)/t_hud_dead \
	$(B)/t_i18n \
	$(B)/t_minimap_lift \
	$(B)/t_loader_weak \
	$(B)/t_mac_overlay \
	$(B)/t_modetx \
	$(B)/t_movie \
	$(B)/t_namegen \
	$(B)/t_oe \
	$(B)/t_padmap \
	$(B)/t_rift_clock \
	$(B)/t_rift_next \
	$(B)/t_rift_oldrun \
	$(B)/t_rift_wave \
	$(B)/t_interaction_diag \
	$(B)/t_map_diag \
	$(B)/t_warp_diag \
	$(B)/t_romfs_validate \
	$(B)/t_rwlv \
	$(B)/t_swkbd \
	$(B)/t_target_rank \
	$(B)/t_textbox \
	$(B)/t_thread_leak \
	$(B)/t_touch \
	$(B)/t_touch_mac \
	$(B)/t_unit_level \
	$(B)/t_widget
	python3 native_write_scan.py --selftest ..
	python3 widget_cache_scan.py --selftest ..
	python3 mode_scan.py ../..
	python3 ../../ios/tools/dm_i18n.py --check
	python3 ../../i18n/scan_literals.py
	$(B)/t_thread_leak
	$(B)/t_loader_weak
	$(B)/t_mac_overlay
	$(PY) test_mac_features.py
	$(B)/t_rift_clock
	$(B)/t_rift_next
	$(B)/t_rift_oldrun
	$(B)/t_interaction_diag
	$(B)/t_map_diag
	$(B)/t_warp_diag
	$(B)/t_rift_wave
	$(B)/t_textbox
	$(B)/t_rwlv
	$(B)/t_touch
	$(B)/t_hid_script 2>/dev/null
	$(B)/t_hid_receipt 2>/dev/null
	$(B)/t_touch_mac
	$(B)/t_oe 2>/dev/null
	$(B)/t_fs 2>/dev/null
	TMPDIR="$(abspath $(B))" $(B)/t_fs_case 2>/dev/null
	DM_ROMFS="$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS" $(B)/t_romfs_validate
	$(B)/t_audio
	$(B)/t_audio_spsc
	$(B)/t_swkbd
	$(B)/t_altstack
	$(B)/t_movie
	$(B)/t_condvar
	$(B)/t_condvar_old
	$(B)/t_target_rank 2>/dev/null
	$(B)/t_hud_dead
	$(B)/t_namegen
	$(B)/t_minimap_lift
	$(B)/t_modetx
	$(B)/t_padmap
	$(B)/t_unit_level
	$(B)/t_widget
	$(B)/t_doorspot
	TMPDIR="$(abspath $(B))" $(B)/t_cloudsync 2>/dev/null
	for m in en zh none sysen systw tags game; do TMPDIR="$(abspath $(B))" $(B)/t_i18n $$m || exit 1; done
	$(PY) movie_cut.py $(B)/main.orig.bin $(B)/subsdk0.orig.bin
	$(PY) oe_notify.py $(B)/main.orig.bin
$(B)/t_i18n: t_i18n.c ../dm_i18n.c ../dm_i18n.h ../dm_i18n_tab.h | $(B)
	clang -O1 -Wall -Werror -I.. -o $@ t_i18n.c ../dm_i18n.c
$(B)/t_mac_overlay: t_mac_overlay.m ../app_mac_overlay.h | $(B)
	clang -O1 -Wall -Werror -fobjc-arc -framework Foundation -o $@ t_mac_overlay.m
$(B)/t_hud_dead: t_hud_dead.c ../dm_hud.h | $(B)
	clang -O1 -Wall -Werror -I.. -o $@ t_hud_dead.c
$(B)/t_textbox: t_textbox.c ../dm_textbox_core.c ../dm_textbox_core.h | $(B)
	clang -O1 -Wall -Werror -o $@ t_textbox.c ../dm_textbox_core.c
$(B)/t_rwlv: t_rwlv.c ../dm_rwlv_core.c ../dm_rwlv_core.h | $(B)
	clang -O1 -Wall -Werror -o $@ t_rwlv.c ../dm_rwlv_core.c
$(B)/t_doorspot: t_doorspot.c ../dm_doorspot.h | $(B)
	clang -O1 -Wall -Werror -I.. -o $@ t_doorspot.c
$(B)/t_widget: t_widget.c ../dm_widget.h | $(B)
	clang -O1 -Wall -Werror -I.. -o $@ t_widget.c
$(B)/t_unit_level: t_unit_level.c ../dm_unit.h | $(B)
	clang -O1 -Wall -Werror -I.. -o $@ t_unit_level.c
$(B)/t_padmap: t_padmap.c ../dm_padmap.h ../dm_input.h | $(B)
	clang -O1 -Wall -Werror -o $@ t_padmap.c
$(B)/t_minimap_lift: t_minimap_lift.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_minimap_lift.c
$(B)/t_modetx: t_modetx.c ../dm_modetx.h | $(B)
	clang -O1 -Wall -Werror -o $@ t_modetx.c
$(B)/t_namegen: t_namegen.c ../dm_namegen.c | $(B)
	clang -O1 -Wall -Werror -o $@ t_namegen.c ../dm_namegen.c
$(B)/t_cloudsync: t_cloudsync.c ../dm_cloudsync.c ../dm.h | $(B)
	clang -O1 -Wall -Werror -I.. -o $@ t_cloudsync.c ../dm_cloudsync.c
$(B)/t_touch: t_touch.c ../hle_touch.c ../front.h ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_touch.c ../hle_touch.c
$(B)/t_fs: t_fs.c t_fs_stub.c ../hle_fs.c ../hle.h ../dm.h ../dm_i18n.c | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_fs.c t_fs_stub.c ../dm_i18n.c
$(B)/t_audio: t_audio.c ../hle_audio.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_audio.c -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
$(B)/t_audio_spsc: t_audio_spsc.c ../hle_audio.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_audio_spsc.c -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
$(B)/t_audio_spsc_tsan: t_audio_spsc.c ../hle_audio.c ../hle.h ../dm.h | $(B)
	clang -O1 -g -O1 -fsanitize=thread -Wno-unused-function -I.. -o $@ t_audio_spsc.c -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
$(B)/t_swkbd: t_swkbd.c ../hle_swkbd.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_swkbd.c ../hle_swkbd.c
$(B)/t_movie: t_movie.c ../hle_movie.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_movie.c ../hle_movie.c
$(B)/t_altstack: t_altstack.c t_altstack_stub.c ../crash.c ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_altstack.c t_altstack_stub.c ../crash.c
$(B)/%.orig.bin: $(EXEFS)/% | $(B)
	$(PY) ../../tools/nso.py $< $@ > /dev/null
$(B):
	mkdir -p $@
.PHONY: run tsan
tsan: $(B)/t_audio_spsc_tsan
	$(B)/t_audio_spsc_tsan
$(B)/t_condvar: t_condvar.c t_condvar_stub.c ../hle_thread.c ../dm.h | $(B)
	clang -O1 -Wno-unused-function -DDM_TEST_HOOKS -I.. -o $@ t_condvar.c t_condvar_stub.c ../hle_thread.c
$(B)/t_condvar_old: t_condvar.c t_condvar_stub.c ../hle_thread.c ../dm.h | $(B)
	clang -O1 -Wno-unused-function -DDM_TEST_HOOKS -DDM_TEST_CV_OLD_ORDER -I.. -o $@ t_condvar.c t_condvar_stub.c ../hle_thread.c

$(B)/t_fs_case: t_fs_case.c ../hle_fs.c ../hle.h ../dm.h ../dm_i18n.c | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_fs_case.c ../dm_i18n.c
$(B)/t_romfs_validate: t_romfs_validate.c ../romfs_validate.c ../romfs_validate.h ../dm_i18n.c ../dm_i18n_tab.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_romfs_validate.c ../romfs_validate.c ../dm_i18n.c

$(B)/t_hid_script: t_hid_script.c t_touch_tick_stub.c ../hle_hid.c ../dm_cursor.c ../dm_hud.c ../dm_hud.h ../touch_mac.c ../../ios/Sources/dm_touch_controls.c ../hle.h ../dm.h ../dm_input.h ../front.h ../dm_cursor.h ../touch_mac.h ../dm_world.h | $(B) ../dm_prof.c
	clang -DDM_PC_ITEMS_CONNECTED=0 -O1 -Wno-unused-function -I.. -I../../ios/Sources -o $@ t_hid_script.c t_touch_tick_stub.c ../dm_cursor.c ../dm_hud.c ../touch_mac.c ../../ios/Sources/dm_touch_controls.c -lm ../dm_prof.c

$(B)/t_oe: t_oe.c ../hle_oe.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_oe.c

$(B)/t_hid_receipt: t_hid_receipt.c t_touch_tick_stub.c ../hle_hid.c ../dm_cursor.c ../dm_hud.c ../dm_hud.h ../touch_mac.c ../../ios/Sources/dm_touch_controls.c ../hle.h ../dm.h ../dm_input.h ../front.h ../dm_cursor.h ../touch_mac.h ../dm_world.h | $(B) ../dm_prof.c
	clang -DDM_PC_ITEMS_CONNECTED=0 -O1 -Wno-unused-function -I.. -I../../ios/Sources -o $@ t_hid_receipt.c t_touch_tick_stub.c ../dm_cursor.c ../dm_hud.c ../touch_mac.c ../../ios/Sources/dm_touch_controls.c -lm ../dm_prof.c
$(B)/t_touch_mac: t_touch_mac.c ../touch_mac.c ../dm_cursor.c ../dm_hud.c ../dm_hud.h ../../ios/Sources/dm_touch_controls.c ../../ios/Sources/dm_touch_controls.h ../../host/dm_input.h ../dm_cursor.h ../touch_mac.h ../dm_world.h | $(B)   # 测试 #include 了 touch_mac.c 与 dm_touch_controls.c，须列为依赖（漏列致改源不重编） ../dm_prof.c | The tests #include touch_mac.c and dm_touch_controls.c, so they must be listed as dependencies (missing them means editing the source does not trigger a rebuild) ../dm_prof.c
	clang -DDM_PC_ITEMS_CONNECTED=0 -std=c11 -Wall -Wextra -Werror -I.. -I../../ios/Sources -o $@ t_touch_mac.c ../dm_cursor.c ../dm_hud.c -lm ../dm_prof.c
$(B)/t_target_rank: t_target_rank.c ../hle_target.c ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_target_rank.c ../hle_target.c
$(B)/t_rift_clock: t_rift_clock.c ../dm_rift.c ../dm_rift.h ../hle_os.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wall -Wno-unused-function -ffunction-sections -fdata-sections -Wl,-dead_strip -I.. -o $@ t_rift_clock.c ../hle_os.c
$(B)/t_rift_next: t_rift_next.c ../dm_rift.c ../dm_rift.h ../hle_os.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wall -Wno-unused-function -ffunction-sections -fdata-sections -Wl,-dead_strip -I.. -o $@ t_rift_next.c ../hle_os.c
$(B)/t_rift_oldrun: t_rift_oldrun.c ../dm_rift.c ../dm_rift.h ../hle_os.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wall -Wno-unused-function -ffunction-sections -fdata-sections -Wl,-dead_strip -I.. -o $@ t_rift_oldrun.c ../hle_os.c
$(B)/t_rift_wave: t_rift_wave.c ../dm_rift.c ../dm_rift.h ../hle_os.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wall -Wno-unused-function -ffunction-sections -fdata-sections -Wl,-dead_strip -I.. -o $@ t_rift_wave.c ../hle_os.c
$(B)/t_thread_leak: t_thread_leak.c ../thread.c ../dm.h | $(B)
	clang -O1 -Wall -Wno-unused-function -I.. -I../../ios/Sources -o $@ t_thread_leak.c

$(B)/t_interaction_diag: t_interaction_diag.c ../dm_world.c ../dm_unit.h ../touch_mac.h | $(B)
	clang -O1 -Wno-unused-function -Wl,-dead_strip -I.. -o $@ t_interaction_diag.c

$(B)/t_map_diag: t_map_diag.c ../dm_cursor.c ../dm_unit.h | $(B)
	clang -O1 -Wno-unused-function -Wl,-dead_strip -I.. -o $@ t_map_diag.c

$(B)/t_warp_diag: t_warp_diag.c ../dm_rift.c ../dm_unit.h | $(B)
	clang -O1 -Wno-unused-function -Wl,-dead_strip -I.. -o $@ t_warp_diag.c

$(B)/t_loader_weak: t_loader_weak.c ../loader.c ../hle.c ../dm.h ../hle.h | $(B)
	clang -O1 -Wno-deprecated-declarations -I.. -Wl,-dead_strip -o $@ t_loader_weak.c ../hle.c

# Synthetic binding labels and AppKit formats; no RomFS or application launch.
.PHONY: controls-i18n-mac
controls-i18n-mac: $(B)/t_controls_i18n_mac
	$(B)/t_controls_i18n_mac

$(B)/t_controls_i18n_mac: t_controls_i18n_mac.m ../dm_controls_mac.m ../dm_controls_mac.h ../dm_i18n.c ../dm_i18n.h ../dm_i18n_tab.h | $(B)
	clang -fobjc-arc -fblocks -Wall -Werror -Wno-unused-function -I.. -framework Cocoa -framework Carbon -o $@ t_controls_i18n_mac.m ../dm_controls_mac.m ../dm_i18n.c

# Input-only PR87 regression suite. Original executable stays in ignored build/.
PC_INPUT_TESTS = t_pc_mouse t_pc_escape t_pc_frontend t_pc_inventory t_pc_keys t_pc_layout t_pc_options_focus t_pc_options_rows t_pc_settings_tabs t_pc_skill_binding t_pc_skill_widgets t_pc_tabbar t_pc_tabbar_mouse t_pc_ui_hover t_pc_uiscreen t_pc_binding_held t_pc_controls_transition t_ui_pointer_queue
.PHONY: pc-input
pc-input: export DM_TEST_MAIN = $(abspath $(B))/main.orig.bin
pc-input: export DM_TEST_BUILD = $(abspath $(B))
pc-input: $(EXEFS)/main $(B)/main.orig.bin $(addprefix $(B)/,$(PC_INPUT_TESTS)) $(B)/t_controls_front $(B)/t_controls_mac $(B)/t_touch_mac controls-i18n-mac t_pc_items_boundary.py test_pc_input_hooks.py t_pc_quickcast_original.py ../../ios/tests/test_pc_input_overlay.py
	@set -e; for test in $(PC_INPUT_TESTS); do if [ "$$test" = t_pc_keys ]; then $(B)/$$test $(B)/main.orig.bin; else $(B)/$$test; fi; done
	@set -e; dir=$$(mktemp -d "$(abspath $(B))/pc-input.XXXXXX"); trap 'rm -r "$$dir"' EXIT; $(B)/t_controls_front "$$dir/front"; for mode in write read resurrected legacy olddefault oldcustom oldcustom-read oldcustom-reset pc34default pc34custom pc34invalid mouse-write mouse-read; do case $$mode in read) data=write;; oldcustom-read|oldcustom-reset) data=oldcustom;; mouse-read) data=mouse-write;; *) data=$$mode;; esac; $(B)/t_controls_mac $$mode "$$dir/$$data"; done
	$(B)/t_touch_mac
	$(PY) t_pc_items_boundary.py
	$(PY) test_pc_input_hooks.py --nso "$(EXEFS)/main"
	$(PY) ../../ios/tests/test_pc_input_overlay.py
	$(PY) ../../ios/tests/test_mac_framework_boundary.py
	$(B)/t_pc_mouse > $(B)/pc-input-mouse.log
	$(PY) t_pc_quickcast_original.py --original $(B)/main.orig.bin --host-log $(B)/pc-input-mouse.log

$(B)/t_pc_escape: t_pc_escape.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_escape.c

$(B)/t_pc_frontend: t_pc_frontend.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_frontend.c

$(B)/t_pc_inventory: t_pc_inventory.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_inventory.c

$(B)/t_pc_keys: t_pc_keys.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_keys.c

$(B)/t_pc_layout: t_pc_layout.c ../dm_padmode.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_layout.c

$(B)/t_pc_options_focus: t_pc_options_focus.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_options_focus.c

$(B)/t_pc_options_rows: t_pc_options_rows.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_options_rows.c

$(B)/t_pc_settings_tabs: t_pc_settings_tabs.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_settings_tabs.c

$(B)/t_pc_skill_binding: t_pc_skill_binding.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_skill_binding.c

$(B)/t_pc_skill_widgets: t_pc_skill_widgets.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_skill_widgets.c

$(B)/t_pc_tabbar: t_pc_tabbar.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_tabbar.c

$(B)/t_pc_tabbar_mouse: t_pc_tabbar_mouse.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_tabbar_mouse.c

$(B)/t_pc_ui_hover: t_pc_ui_hover.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_ui_hover.c

$(B)/t_pc_uiscreen: t_pc_uiscreen.c ../dm_uiscreen.c ../dm_uiscreen.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_uiscreen.c

$(B)/t_pc_binding_held: t_pc_binding_held.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_binding_held.c

$(B)/t_pc_controls_transition: t_pc_controls_transition.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_controls_transition.c

$(B)/t_ui_pointer_queue: t_ui_pointer_queue.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_ui_pointer_queue.c

$(B)/t_controls_front: t_controls_front.m ../front_mac.m ../dm_controls_mac.m ../dm_controls_mac.h ../dm_i18n.c ../dm_i18n_tab.h ../touch_mac.h | $(B)
	clang -O1 -fobjc-arc -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -I.. -Wl,-undefined,dynamic_lookup -o $@ t_controls_front.m ../dm_controls_mac.m ../dm_i18n.c -framework Cocoa -framework Carbon -framework QuartzCore -framework Metal -framework ImageIO

$(B)/t_controls_mac: t_controls_mac.m ../dm_controls_mac.m ../dm_controls_mac.h ../dm_i18n.c ../dm_i18n_tab.h | $(B)
	clang -O1 -fobjc-arc -Wall -Werror -Wno-unused-function -I.. -o $@ t_controls_mac.m ../dm_controls_mac.m ../dm_i18n.c -framework Cocoa -framework Carbon

$(B)/t_pc_mouse: t_pc_mouse.c ../dm_world.c ../dm_world.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_mouse.c
