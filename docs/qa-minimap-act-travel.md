# Touch minimap recovery after NPC Act travel (#49)

## Evidence boundary

The reporter now identifies a repeatable path: NPC travel Act I -> II or Act II -> III hides the top-right minimap while the world remains normal, reportedly still on the new version. The reattached file is the same build 10150343 log, without the newer minimap diagnostics. Public source reviewed: 1784563. Source confirms an implementation gap: touch UI promises an always-visible minimap, but `ui_map_pump` reconciles mode only and does not reopen a hidden AutoMap after travel. This does not establish which native travel callback hides it, or prove that every missing map is a hidden widget.

## Patch

At the existing 30-frame minimap poll, reconcile a hidden, attached AutoMap through `PanelManager:OpenPanel AutoMap`. Wait for explicit loading completion, a valid living player, a visible HUD, touch mode outside a switching transaction, and no visible nonresident menu. Missing/unrecognized map widgets wait for the original layout to reattach. Preserve map mode, coordinates, zoom, discovered data and controller behavior. Wait one sample before requesting recovery, then at least 60 guest frames between attempts, with at most three attempts per continuous hidden episode. Mode leave clears pending state through the behavior registry; the additional message dispatch is registered in the mode gate (38 -> 39).

This is a visibility-recovery patch, not a proven native root-cause correction. If a missing map has visible AutoMap already, this patch deliberately does not fire; investigate its renderer/contents with the existing snapshots instead.

## Executed checks

The fixture includes production `dm_cursor.c`, actual minimap poll and actual `fe_send`, with authored widget trees and a fake PanelManager receiver. The initial core fixture failed 3/12 checks before the patch. The expanded final fixture passes 18/18, including loading, controller, busy transaction, menu, death, invalid player, out-of-game, Act II/III areas, retry limit, mode cleanup, inert FocusManager, unavailable HUD/widget and unchanged geometry/mode. UBSan passes. Existing minimap diagnostics pass 115 checks. Literal/i18n, widget-write selftest, mode registration, C syntax and diff checks pass.

The receiver simulates the native manager opening the panel. This is not a native game, iOS build, GPU rendering or real-device result. No original executable/RomFS is available here; no post-change game screenshot is claimed. No game content is committed.

## Maintainer AI / device gate

Keep this PR draft until native verification. Build current source and matching shell. First capture the unpatched NPC Act I -> II and II -> III sequence with the newer `小地图诊断` records and screenshots. Determine whether AutoMap is hidden, absent, or visible with missing content. Then repeat patched:

1. Travel through NPCs I -> II -> III, and back; the map should return after the loading screen and dialogue close, with no repeated opening while visible.
2. Repeat first entry, repeated travel, save/reload and lock/unlock. Verify III's city layout and minimap markings as well as II.
3. Open inventory/merchant/pause during arrival and switch to/from controller: no OpenPanel should be sent while blocked; touch recovery can resume afterwards. Preserve controller map toggle and runtime fullscreen selection.
4. Verify `小地图恢复` and `小地图诊断` correlate: recovery stops after visibility returns. If three requests fail, preserve the log and inspect the native panel-manager/map-open state; do not increase retries or directly write the visibility bit as a substitute.
5. Check actual visible lines, clipping, hit zone, map toggling, explored content and UI focus; attach before/after native screenshots. Host tests cannot establish these.
