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
	$(B)/t_audio_focus \
	$(B)/t_calibration \
	$(B)/t_cloudsync \
	$(B)/t_custom_lang \
	$(B)/t_custom_scroll \
	$(B)/t_controls_mac \
	$(B)/t_controls_front \
	$(B)/t_input_probe_mac \
	$(B)/t_controls_pc_panels \
	$(B)/t_controls_ui \
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
	$(MAKE) controls
	$(B)/t_calibration
	$(B)/t_custom_scroll
	$(B)/t_audio_focus
	$(PY) t_audio_focus_native.py $(B)/main.orig.bin
	$(PY) t_pc_ui_native.py $(B)/main.orig.bin
	$(PY) t_pc_cursor_resources.py $(B)/main.orig.bin
	@probe_dir=$$(mktemp -d /tmp/d2r-probe-tests.XXXXXX); $(B)/t_input_probe_mac "$$probe_dir"
	$(PY) t_pc_hover_gate.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	$(PY) t_pc_gates_r48.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	$(PY) t_pc_inventory_gate.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	$(PY) t_pc_options_gates.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	$(PY) t_pc_controller_gates.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	$(PY) t_pc_pause_close_gate.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	$(PY) t_pc_item_left_gate.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	$(PY) t_pc_item_npc_gates.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	cd ../../.. && $(PY) dm/host/tests/t_pc_item_right_relocation.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	$(PY) t_pc_held_draw_gate.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	cd ../../.. && $(PY) dm/host/tests/t_pc_item_tooltip_gate.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	cd ../../.. && $(PY) dm/host/tests/t_pc_grid_draw_obs.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	cd ../../.. && $(PY) dm/host/tests/t_pc_grid_hover_gate.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	cd ../../.. && $(PY) dm/host/tests/t_pc_create_commit_gate.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	cd ../../.. && $(PY) dm/host/tests/t_pc_create_commit_original.py
	cd ../../.. && $(PY) dm/host/tests/t_pc_skill_mouse_gates.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	cd ../../.. && $(PY) dm/host/tests/t_pc_skill_filter_gates.py "$(DM_ROOT)/build/cache/dm/out/$(shell cat ../../PRODUCT)-touch"
	cd ../../.. && DM_TEST_ROMFS="$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS" $(PY) dm/ios/tools/test_spanish_item_notes.py
	cd ../../.. && DM_TEST_ROMFS="$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS" $(PY) dm/ios/tools/test_pause_logo.py
	cd ../../.. && DM_TEST_ROMFS="$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS" $(PY) dm/ios/tools/test_pc_monster_health.py
	python3 native_write_scan.py --selftest ..
	python3 widget_cache_scan.py --selftest ..
	python3 mode_scan.py ../..
	python3 ../../ios/tools/dm_i18n.py --check
	python3 ../../i18n/scan_literals.py
	$(B)/t_thread_leak
	$(B)/t_loader_weak
	$(B)/t_rift_clock
	$(B)/t_rift_next
	$(B)/t_rift_oldrun
	$(B)/t_interaction_diag
	$(B)/t_map_diag
	$(B)/t_warp_diag
	$(B)/t_rift_wave
	$(B)/t_textbox
	$(B)/t_collection_names
	$(B)/t_collection_scope
	$(PY) t_collection_parent_original.py $(B)/main.orig.bin
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
	for m in es esmx mac legacyen legacyzh legacyzh_tw legacyzh_es legacy_mix_9_2 legacy_mix_2_9 precedence none invalid; do $(B)/t_custom_lang $$m || exit 1; done
	for m in en zh es esmx none sysen syses systw startup tags game; do TMPDIR="$(abspath $(B))" $(B)/t_i18n $$m || exit 1; done
	$(PY) movie_cut.py $(B)/main.orig.bin $(B)/subsdk0.orig.bin
	$(PY) oe_notify.py $(B)/main.orig.bin
controls: mac-cursors $(B)/t_controls_mac $(B)/t_controls_ui $(B)/t_controls_front $(B)/t_controls_pc_panels $(B)/t_display_mac $(B)/t_cursor_mac $(B)/t_pc_layout $(B)/t_pc_pouch $(B)/t_pc_skill_binding $(B)/t_pc_world_hit $(B)/t_pc_mouse $(B)/t_pc_uiscreen $(B)/t_pc_keys $(B)/t_ui_pointer_queue $(B)/t_pc_tabbar $(B)/t_pc_settings_tabs $(B)/t_pc_inventory $(B)/t_pc_tabbar_mouse $(B)/t_pc_frontend $(B)/t_pc_options_focus $(B)/t_pc_options_rows $(B)/t_pc_world_trace $(B)/t_pc_escape $(B)/t_pc_ui_hover
	$(B)/t_cursor_mac "$(DM_ROOT)/build/cache/mac-cursors"
	$(B)/t_pc_layout
	$(B)/t_pc_pouch
	$(B)/t_pc_skill_binding
	$(B)/t_pc_world_hit
	$(B)/t_pc_mouse $(B)/main.orig.bin > $(B)/pc_mouse_original.log
	cat $(B)/pc_mouse_original.log
	cd ../../.. && $(PY) dm/host/tests/t_pc_quickcast_original.py --host-log dm/host/tests/$(B)/pc_mouse_original.log
	$(B)/t_pc_uiscreen
	$(B)/t_pc_keys $(B)/main.orig.bin
	$(B)/t_ui_pointer_queue
	$(B)/t_pc_tabbar
	$(B)/t_pc_tabbar_mouse
	$(B)/t_pc_settings_tabs
	$(B)/t_pc_inventory
	$(B)/t_pc_frontend
	$(B)/t_pc_escape
	$(B)/t_pc_controls_transition
	$(B)/t_pc_tooltip_trace
	$(B)/t_pc_item_tooltip
	$(B)/t_pc_stash_pages $(B)/pc_stash_callbacks.bin
	$(B)/t_pc_tooltip_draw_trace
	$(B)/t_pc_personal_button_trace
	$(B)/t_pc_grid_draw_entry
	$(B)/t_pc_grid_hover
	$(B)/t_pc_tooltip_domains
	cd ../../.. && dm/host/tests/$(B)/t_pc_skill_widgets
	cd ../../.. && dm/host/tests/$(B)/t_pc_binding_held
	$(B)/t_pc_create_commit
	$(B)/t_pc_character_form
	$(B)/t_pc_options_focus
	$(B)/t_pc_ui_hover
	$(B)/t_pc_item_left
	$(B)/t_pc_item_hold
	$(B)/t_pc_held_draw
	$(B)/t_pc_npc_click
	$(B)/t_pc_vault_layout
	$(B)/t_pc_vault_pointer
	$(B)/t_pc_vault_pointer_widgets
	$(B)/t_pc_vault_deposit
	$(B)/t_pc_options_rows
	$(B)/t_pc_world_trace
	@fixture_dir=$$(mktemp -d /tmp/d2r-controls-tests.XXXXXX); $(B)/t_controls_mac write "$$fixture_dir" && $(B)/t_controls_mac read "$$fixture_dir" && $(B)/t_controls_mac resurrected "$$fixture_dir/new" && $(B)/t_controls_mac olddefault "$$fixture_dir/olddefault" && $(B)/t_controls_mac resurrected "$$fixture_dir/olddefault" && $(B)/t_controls_mac oldcustom "$$fixture_dir/oldcustom" && $(B)/t_controls_mac oldcustom-read "$$fixture_dir/oldcustom" && $(B)/t_controls_mac oldcustom-reset "$$fixture_dir/oldcustom" && $(B)/t_controls_mac resurrected "$$fixture_dir/oldcustom" && $(B)/t_controls_mac pc34default "$$fixture_dir/pc34default" && $(B)/t_controls_mac pc34custom "$$fixture_dir/pc34custom" && $(B)/t_controls_mac pc34invalid "$$fixture_dir/pc34invalid" && $(B)/t_controls_mac legacy "$$fixture_dir/legacy" && $(B)/t_controls_mac glyph "$$fixture_dir/glyph" && $(B)/t_controls_mac mouse-write "$$fixture_dir/mouse" && $(B)/t_controls_mac mouse-read "$$fixture_dir/mouse" && $(B)/t_controls_ui "$$fixture_dir/ui" "$$fixture_dir/panel.png" && $(B)/t_controls_front "$$fixture_dir/front" && $(B)/t_controls_pc_panels && $(B)/t_display_mac "$$fixture_dir/display"
$(B)/t_controls_mac: t_controls_mac.m ../dm_controls_mac.m ../dm_controls_mac.h ../touch_mac.h | $(B)
	clang -O1 -fobjc-arc -Wall -Werror -Wno-deprecated-declarations -I.. -o $@ t_controls_mac.m ../dm_controls_mac.m -framework Cocoa -framework Carbon
$(B)/t_cursor_mac: t_cursor_mac.m ../front_mac.m ../dm_controls_mac.m | $(B)
	clang -O1 -fobjc-arc -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -I.. -Wl,-undefined,dynamic_lookup -Wl,-dead_strip -o $@ t_cursor_mac.m ../dm_controls_mac.m -framework Cocoa -framework Carbon -framework QuartzCore -framework Metal -framework ImageIO
$(B)/t_pc_layout: t_pc_layout.c ../dm_padmode.c ../dm.h ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -I.. -Wl,-dead_strip -o $@ t_pc_layout.c
$(B)/t_pc_skill_binding: t_pc_skill_binding.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_skill_binding.c
$(B)/t_pc_world_hit: t_pc_world_hit.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_world_hit.c
$(B)/t_pc_mouse: t_pc_mouse.c ../dm_world.c ../dm_world.h ../dm_cursor.h ../touch_mac.h | $(B)
	clang -O1 -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_mouse.c
$(B)/t_pc_uiscreen: t_pc_uiscreen.c ../dm_uiscreen.c ../dm_uiscreen.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_uiscreen.c
$(B)/t_ui_pointer_queue: t_ui_pointer_queue.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_ui_pointer_queue.c
$(B)/t_pc_keys: t_pc_keys.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_keys.c
$(B)/t_pc_pouch: t_pc_pouch.c ../dm_pouch.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -Wl,-undefined,dynamic_lookup -o $@ t_pc_pouch.c
$(B)/t_controls_pc_panels: t_controls_pc_panels.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_controls_pc_panels.c
$(B)/t_calibration: t_calibration.c calibration_messages.tsv ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_calibration.c
$(B)/t_audio_focus: t_audio_focus.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_audio_focus.c
$(B)/t_custom_scroll: t_custom_scroll.c ../dm_custom.c ../dm_custom.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_custom_scroll.c
$(B)/t_controls_front: t_controls_front.m ../front_mac.m ../dm_controls_mac.m ../dm_controls_mac.h ../touch_mac.h | $(B)
	clang -O1 -fobjc-arc -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -I.. -Wl,-undefined,dynamic_lookup -o $@ t_controls_front.m ../dm_controls_mac.m -framework Cocoa -framework Carbon -framework QuartzCore -framework Metal -framework ImageIO
$(B)/t_input_probe_mac: t_input_probe_mac.m ../front_mac.m ../dm_controls_mac.h ../touch_mac.h | $(B)
	clang -O1 -fobjc-arc -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -I.. -Wl,-undefined,dynamic_lookup -Wl,-dead_strip -o $@ t_input_probe_mac.m -framework Cocoa -framework Carbon -framework QuartzCore -framework Metal -framework ImageIO
$(B)/t_display_mac: t_display_mac.m t_display_cursor.c ../front_mac.m ../dm_controls_mac.m ../dm_controls_mac.h ../touch_mac.h ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -fobjc-arc -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -I.. -Wl,-undefined,dynamic_lookup -Wl,-dead_strip -o $@ t_display_mac.m t_display_cursor.c ../dm_controls_mac.m -framework Cocoa -framework Carbon -framework QuartzCore -framework Metal -framework ImageIO
$(B)/t_controls_ui: t_controls_ui.m ../dm_controls_mac.m ../dm_controls_mac.h ../touch_mac.h | $(B)
	clang -O1 -fobjc-arc -Wall -Werror -Wno-deprecated-declarations -I.. -o $@ t_controls_ui.m -framework Cocoa -framework Carbon
$(B)/t_custom_lang: t_custom_lang.c ../dm_custom.c ../dm_custom.h ../dm_i18n.c ../dm_i18n.h ../dm_i18n_tab.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -Wl,-dead_strip -I.. -o $@ t_custom_lang.c ../dm_custom.c ../dm_i18n.c
$(B)/t_i18n: t_i18n.c ../dm_i18n.c ../dm_i18n.h ../dm_i18n_tab.h | $(B)
	clang -O1 -Wall -Werror -I.. -o $@ t_i18n.c ../dm_i18n.c
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

$(B)/t_hid_script: t_hid_script.c t_touch_tick_stub.c ../hle_hid.c ../dm_cursor.c ../dm_cursor.h ../dm_hud.c ../dm_hud.h ../touch_mac.c ../touch_mac.h ../../ios/Sources/dm_touch_controls.c ../hle.h ../dm.h ../dm_input.h ../front.h ../dm_world.h | $(B) ../dm_prof.c
	clang -O1 -Wno-unused-function -I.. -I../../ios/Sources -o $@ t_hid_script.c t_touch_tick_stub.c ../dm_cursor.c ../dm_hud.c ../touch_mac.c ../../ios/Sources/dm_touch_controls.c -lm ../dm_prof.c

$(B)/t_oe: t_oe.c ../hle_oe.c ../hle.h ../dm.h | $(B)
	clang -O1 -Wno-unused-function -I.. -o $@ t_oe.c

$(B)/t_hid_receipt: t_hid_receipt.c t_touch_tick_stub.c ../hle_hid.c ../dm_cursor.c ../dm_cursor.h ../dm_hud.c ../dm_hud.h ../touch_mac.c ../touch_mac.h ../../ios/Sources/dm_touch_controls.c ../hle.h ../dm.h ../dm_input.h ../front.h ../dm_world.h | $(B) ../dm_prof.c
	clang -O1 -Wno-unused-function -I.. -I../../ios/Sources -o $@ t_hid_receipt.c t_touch_tick_stub.c ../dm_cursor.c ../dm_hud.c ../touch_mac.c ../../ios/Sources/dm_touch_controls.c -lm ../dm_prof.c
$(B)/t_touch_mac: t_touch_mac.c ../touch_mac.c ../touch_mac.h ../dm_cursor.c ../dm_cursor.h ../dm_hud.c ../dm_hud.h ../../ios/Sources/dm_touch_controls.c ../../ios/Sources/dm_touch_controls.h ../../host/dm_input.h ../dm_world.h | $(B)   # 测试 #include 了 touch_mac.c 与 dm_touch_controls.c，须列为依赖（漏列致改源不重编） ../dm_prof.c | The tests #include touch_mac.c and dm_touch_controls.c, so they must be listed as dependencies (missing them means editing the source does not trigger a rebuild) ../dm_prof.c
	clang -std=c11 -Wall -Wextra -Werror -I.. -I../../ios/Sources -o $@ t_touch_mac.c ../dm_cursor.c ../dm_hud.c -lm ../dm_prof.c
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

$(B)/t_pc_tabbar: t_pc_tabbar.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_tabbar.c

$(B)/t_pc_settings_tabs: t_pc_settings_tabs.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_settings_tabs.c

$(B)/t_pc_inventory: t_pc_inventory.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_inventory.c

$(B)/t_pc_tabbar_mouse: t_pc_tabbar_mouse.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_tabbar_mouse.c

$(B)/t_pc_frontend: t_pc_frontend.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_frontend.c

controls: $(B)/t_pc_vault_layout $(B)/t_pc_vault_pointer $(B)/t_pc_vault_pointer_widgets $(B)/t_pc_vault_deposit $(B)/t_pc_item_left $(B)/t_pc_item_hold $(B)/t_pc_npc_click $(B)/t_pc_held_draw

$(B)/t_pc_held_draw: t_pc_held_draw.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_held_draw.c

$(B)/t_pc_item_left: t_pc_item_left.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_item_left.c

$(B)/t_pc_item_hold: t_pc_item_hold.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_item_hold.c

$(B)/t_pc_npc_click: t_pc_npc_click.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_npc_click.c

$(B)/t_pc_vault_layout: t_pc_vault_layout.c ../dm_vault_ui.c | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_vault_layout.c

$(B)/t_pc_vault_pointer: t_pc_vault_pointer.c ../dm_vault_ui.c | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_vault_pointer.c

$(B)/t_pc_vault_pointer_widgets: t_pc_vault_pointer_widgets.c ../dm_vault_ui.c | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_vault_pointer_widgets.c

$(B)/t_pc_vault_deposit: t_pc_vault_deposit.c ../dm_vault_ui.c | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_vault_deposit.c

$(B)/t_pc_options_focus: t_pc_options_focus.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_options_focus.c

$(B)/t_pc_ui_hover: t_pc_ui_hover.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_ui_hover.c

$(B)/t_pc_options_rows: t_pc_options_rows.c ../dm_cursor.c ../dm_world.c ../dm_cursor.h ../dm_world.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_options_rows.c

$(B)/t_pc_world_trace: t_pc_world_trace.c ../dm_cursor.c ../dm_world.c ../dm_cursor.h ../dm_world.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_world_trace.c

$(B)/t_pc_escape: t_pc_escape.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_escape.c

controls: $(B)/t_pc_controls_transition
run: $(B)/t_collection_names $(B)/t_collection_scope

$(B)/t_pc_controls_transition: t_pc_controls_transition.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_controls_transition.c

$(B)/t_collection_names: t_collection_names.c ../dm_textbox_core.c ../dm_textbox_core.h | $(B)
	clang -O1 -Wall -Werror -o $@ t_collection_names.c ../dm_textbox_core.c

$(B)/t_collection_scope: t_collection_scope.c ../dm_textbox.c ../dm_textbox_core.c ../dm_textbox_core.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_collection_scope.c ../dm_textbox_core.c

controls: $(B)/t_pc_tooltip_trace $(B)/t_pc_item_tooltip

controls: $(B)/t_pc_stash_pages

controls: $(B)/t_pc_tooltip_draw_trace

$(B)/t_pc_tooltip_draw_trace: t_pc_tooltip_draw_trace.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_tooltip_draw_trace.c

$(B)/t_pc_stash_pages: t_pc_stash_pages.c ../dm_cursor.c ../diag.c | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -I.. -Wl,-dead_strip -o $@ t_pc_stash_pages.c

$(B)/t_pc_tooltip_trace: t_pc_tooltip_trace.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_tooltip_trace.c

$(B)/t_pc_item_tooltip: t_pc_item_tooltip.c ../dm_cursor.c ../dm_cursor.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_item_tooltip.c

controls: $(B)/t_pc_character_form
$(B)/t_pc_character_form: t_pc_character_form.c ../dm_cursor.c ../dm.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_character_form.c

controls: $(B)/t_pc_grid_draw_entry
$(B)/t_pc_grid_draw_entry: t_pc_grid_draw_entry.c ../dm_cursor.c ../dm.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_grid_draw_entry.c

controls: $(B)/t_pc_personal_button_trace
$(B)/t_pc_personal_button_trace: t_pc_personal_button_trace.c ../dm_cursor.c ../dm.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_personal_button_trace.c

controls: $(B)/t_pc_grid_hover
$(B)/t_pc_grid_hover: t_pc_grid_hover.c ../dm_cursor.c ../dm.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_grid_hover.c

controls: $(B)/t_pc_create_commit
$(B)/t_pc_create_commit: t_pc_create_commit.c ../dm_cursor.c ../dm.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_create_commit.c

controls: $(B)/pc_stash_callbacks.bin
$(B)/pc_stash_callbacks.bin: t_pc_stash_callback_original.py ../../ios/tools/pc_desktop_layout.py $(B)/main.orig.bin | $(B)
	cd ../../.. && $(PY) dm/host/tests/t_pc_stash_callback_original.py dm/host/tests/$(B)/pc_stash_callbacks.bin

controls: $(B)/t_pc_tooltip_domains
$(B)/t_pc_tooltip_domains: t_pc_tooltip_domains.c ../dm_cursor.c ../dm.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_tooltip_domains.c

controls: $(B)/t_pc_skill_widgets
$(B)/t_pc_skill_widgets: t_pc_skill_widgets.c ../dm_cursor.c ../dm.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_skill_widgets.c

controls: $(B)/t_pc_binding_held
$(B)/t_pc_binding_held: t_pc_binding_held.c ../dm_cursor.c ../dm_cursor.h ../dm.h | $(B)
	clang -O1 -Wall -Werror -Wno-unused-function -I.. -Wl,-dead_strip -o $@ t_pc_binding_held.c

.PHONY: mac-cursors
mac-cursors:
	$(PY) ../../ios/tools/gen_mac_cursors.py "$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS" "$(DM_ROOT)/build/cache/mac-cursors"

../dm_vault_tab.h:
	$(MAKE) -C .. dm_vault_tab.h DM_ROOT="$(DM_ROOT)"
$(B)/t_pc_vault_layout $(B)/t_pc_vault_pointer_widgets $(B)/t_pc_vault_deposit: ../dm_vault_tab.h

$(B)/t_movie_real: t_movie_real.c ../hle_movie.c ../dm_movie_decode.c ../dm_movie_decode.h | $(B)
	clang -O1 -Wno-deprecated-declarations -DDM_MOVIE_FFMPEG=1 $$(pkg-config --cflags libavformat libavcodec libavutil libswscale) -I.. -Wl,-dead_strip -o $@ t_movie_real.c ../dm_movie_decode.c $$(pkg-config --libs libavformat libavcodec libavutil libswscale)
.PHONY: movie-real
movie-real: $(B)/t_movie_real
	@test -n "$(MOVIE_WEBM)" -a -n "$(MOVIE_NV12)" || { echo 'Indica MOVIE_WEBM y MOVIE_NV12 locales'; exit 2; }
	$(B)/t_movie_real "$(MOVIE_WEBM)" "$(MOVIE_NV12)"
