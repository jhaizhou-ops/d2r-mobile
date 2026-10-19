# Mac PC input profile

Enable the optional desktop profile with `DM_OVERLAY_PC=1` when generating the Mac overlay/package. The default controller and iOS paths retain their existing layouts. This profile uses original PC layouts when present, original keyboard binding records and original action dispatchers; it does not turn mouse clicks into touch taps.

## Integration prerequisites

This source slice keeps `dm/PRODUCT` unchanged at r44 as requested. **Old cached r44 products do not contain these hooks.** Maintainers must regenerate rewritten products from this checkout before pairing them with the new host. Preserve existing installations and use a new private workspace:

```sh
DM_ROOT="/path/to/new-private-workspace" \
DM_NSO_DIR="/path/to/own/ExeFS" \
DM_PY="/path/to/prepared/venv/bin/python" \
  bash dm/tools/product.sh build
```

This command is an integration recipe, not a claim that this slice regenerated or ran a game. Use the same new `DM_ROOT` for subsequent builds. Game resources, shader libraries and other existing setup prerequisites are still required; this change supplies no game files.

PR #95 is the source of the non-PC coalesced mouse-hover path. It was still open when this slice was prepared. Keep that behavior once #95 is integrated: operation 3 is hover in the legacy path, while right-button down uses the explicit mouse API. In PC mode pointer events retain their exact floating-point coordinates in the ordered queue. The small shared hover helper is attributed to #95; it is not a new hover contribution.

## Review map

| Boundary | Implementation |
| --- | --- |
| AppKit events, modifiers and text focus | `dm/host/front_mac.m`, `dm_controls_mac.[hm]` |
| Ordered pointer events, cancellation and legacy hover | `dm/host/touch_mac.[ch]`, `dm_cursor.c` |
| Original 69-action binding matcher/dispatcher and held state | `dm/host/dm_cursor.c` |
| Point-and-click movement, targeting and quick cast | `dm/host/dm_world.c` |
| PC layout fallback and readiness | `dm/host/dm_padmode.c`, `dm/ios/tools/pc_desktop_layout.py` |
| Guarded guest call sites and resource arguments | `dm/tools/patch_nso.py`, `dm/host/hle*`, `dm.h` |

The AppKit fallback scheme offers skills 1–6 and potions QWER. In the active PC profile, the original game binding records remain authoritative and its own Controls page handles reassignment. Stand-still, secondary mouse bindings, wheel bindings, quick-cast selection and releases use the native action paths. Focus loss, mode changes and queue cancellation invalidate held input before another context can consume it.

The optional item module is a separate slice. When its header/source are present, the pointer pump publishes a guest-thread snapshot containing root identity, UI generation, cancellation epoch, exact coordinates, modifiers and readiness. It processes item events before/after the native root handler. The input slice builds without it. The shared A3 grid gate has a weak skill-only implementation here; the item module overrides it and calls the skill handler before its own grid handler. Keep one A3 registration when porting both slices.

Character creation translates the PC `Create` message into the original Switch `TryCreateCharacter` operation so the native name validation/copy still runs. It does not alter entitlement checks. The desktop generator retains original character layouts and language/profile metrics; it adds no private class controls or Spanish table column.

Cursor PNGs are extracted from the caller's own RomFS by `gen_mac_cursors.py` and placed in the private Mac bundle when `DM_OVERLAY_PC=1`. Linked mode instead records an external `cursors` directory in `paths.txt`, keeping game-derived images outside the app. No cursor artwork is committed. `DM_MAC_CURSOR_DIR` can select an extracted directory for a development host. The cursor hotspot is independent of event coordinate conversion.

## Checks

With the existing prepared environment containing Unicorn, Pillow and the project's NSO dependencies:

```sh
make -C dm/host/tests pc-input \
  DM_ROOT="/path/to/prepared-private-workspace" \
  PY="/path/to/prepared/venv/bin/python"
python3 dm/ios/tests/test_mac_framework_boundary.py
python3 dm/ios/tools/dm_i18n.py --check
python3 dm/host/tests/native_write_scan.py --selftest dm/host
python3 dm/host/tests/widget_cache_scan.py --selftest dm/host
python3 dm/host/tests/mode_scan.py dm
```

`pc-input` covers queue precision/order/cancellation, native binding tables, mouse/quick-cast predicates, skill hover, controls transitions, AppKit persistence/localization and legacy #95 hover. Its ARM checks use the caller's own original executable and explicit providers; they do not run the game, GPU or server. The overlay test packs synthetic resources and verifies CASC readback. The framework test confirms Carbon/Cocoa stay out of generated iOS linking inputs. A custom `B` output directory is supported.

The unchanged literal scanner has a baseline discrepancy: system Python 3.9 reports two pre-existing f-string diagnostics; Python 3.12+ reports success because its tokenizer exposes fragments the current scanner does not inspect. Do not interpret that newer-Python result as complete f-string coverage.

Full rewritten-product regression, a newly launched game, visual behavior on Mac, and physical iPhone smoke remain maintainer integration checks. The accepted installed application was not changed or launched for this extraction. No universal compatibility or visual certification is claimed by the source fixtures.
