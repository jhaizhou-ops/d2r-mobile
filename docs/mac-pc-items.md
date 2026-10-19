# PC inventory, NPC dialogue and stash boundary

中文: [mac-pc-items.zh-CN.md](mac-pc-items.zh-CN.md)

This slice separates the item-related PC adaptations from the input frontend. It contains no game resources, generated rewrite output, saves, Spanish translation, movie player, character creation or keyboard-binding editor. It targets the original Switch program **3.3.94012**; other program versions fail the instruction guards.

## Integration status

The code builds on the main branch without the PC-input slice. In that configuration its weak `dm_ui_pc_desktop()` returns zero: the new gates preserve the original paths and no PC item behavior is enabled. This is a build-independent module, **not a functionally independent desktop port**.

Enabling it requires the PC-input/profile work, including root-first mouse dispatch and the profile/layout hooks. That work supplies a strong `dm_ui_pc_desktop()` and publishes the snapshot below. Existing PR95 physical hover is a prerequisite of the input integration, not a substitute for these item hooks. A strong optional `dm_ui_pc_skill_grid_hover(regs)` lets the input slice preserve its skill-selector hover before the item-specific A3 decision; the standalone fallback returns zero.

The host source and rewriter must be integrated together. `dm/PRODUCT` is deliberately unchanged. An existing r44 cache does **not** acquire these hooks merely by rebuilding the host, and `setup.sh` may reuse an existing cache. The maintainer must explicitly rebuild the selected product and run `dm/verify/regress.sh --fresh` before accepting it. Use a separate `DM_ROOT` for a candidate so an accepted installation/cache is preserved.

## Ownership and event contract

`dm/host/dm_pc_items.[ch]` owns item policy, deferred drag state and the seven host gates. A3 is a shared ABI with the input slice: merge one gate/enum/dispatch row rather than installing two stubs. The input module owns AppKit events, the pointer queue, root/profile selection and the epoch/generation lifecycle.

All publication, pointer dispatch, pumping and UI-message observation run on the game thread. Only `dm_pc_items_cancel()` may run on another thread; it increments an atomic cancellation serial without dereferencing guest pointers.

1. Publish `dm_pc_items_snapshot` with the current root, generation, cancellation epoch, native UI-input pixel coordinates/dimensions, classic dimensions, modifiers, HD flag and game/loading/busy state. `event_scope` is 0 for a persistent pointer, 1 for the current physical event, and 2 for invalid input. `valid` must include viewport/geometry validation. Do not mark an old queue event current after blur or root replacement.
2. For a physical event, call `dm_pc_items_pointer(phase,x,y)` before the normal root dispatch. Use the existing `DM_UP_*` phase values. A nonzero result consumes the event; clear the publisher's own LUP/RUP tracking when that release is consumed.
3. Deliver unconsumed events through the original root, then call `dm_pc_items_pointer_after()`. This records a drag release; placement waits for the matching held-item acknowledgement and remaps the saved release point against the current target immediately before its native F8 call.
4. Call `dm_pc_items_pump()` after publishing the current snapshot. Do not treat a queued placement as a second mouse click. The acknowledgement wait is bounded at 90 pumps.
5. Forward parsed UI messages to `dm_pc_items_message(category,action,argument)`. Personal paging accepts only the native `PanelManager:ClosePanel:TouchPersonalStashPrevious/Next` protocol; it requires personal tab zero, an empty hand and valid context. Shared tabs retain their original callbacks.
6. Cancel on blur, mode/root replacement and pointer cancellation. A canceled publication cannot rearm until a strictly newer epoch is published, including after repeated cancels or absent snapshots. Root/generation/epoch changes invalidate deferred placements.

Held drawing changes only the argument passed to the original renderer. HD uses the published input point; classic uses the original `23e760` projection once after checking the classic dimensions. The module does not write original focus/selection widgets or global controller mode. Held-item retention is passive and uses the current native queue/inventory/panel context, so blur or an invalid pointer cannot itself authorize an automatic drop; physical actions and drawing still require a valid snapshot.

## Native behavior retained

| Boundary | Behavior |
|---|---|
| ItemSlot F8 | LMB pickup/place/swap remains the native class operation; empty, occupied and invalid destinations retain native eligibility and client/server processing |
| Modifiers | Ctrl transfers to/from bank or cube, sells through native trade, or requests Chronicle deposit; Shift uses native belt eligibility/placement, bulk purchase or the existing mercenary-potion API |
| NPC rows | Physical LMB on a live member of the current NPC list enters the original child dispatch with its `DialogMessage:ListItemClicked` callback; it does not synthesize ControllerAction |
| Held reconciliation | An active item panel preserves held items against controller auto-reconciliation, including blur and viewport exit; the original queue and explicit drop operations remain intact |
| Tooltips and trade | Scoped inventory/equipment/bank/vendor getter callers use the current physical hover; the native formatter, price, purchase and confirmation functions remain responsible for the result |
| Chronicle/Vault | Own widgets retain pointer capture, drag cancellation, deposit-return identity and native withdrawal requests; legacy touch/controller geometry remains selected outside the PC profile |

Bank/shop/cube/equipment families share the original ItemSlot dispatch; the module identifies the supported native F8 functions rather than trusting layout names. This is not a replacement inventory or a host-side item simulator.

## Rewrite contract

All seven cuts are private touch-product gates. Public/native and rules-only products remain unmodified by these additions.

| SVC | Original site | Decision |
|---|---|---|
| 9d | `3ca47c` | ItemSlot physical LMB; original consumed branch |
| 9e | `3ca58c` | Shift RMB; otherwise original virtual call and LR |
| 9f | `3b8648` | Held-item controller reconciliation |
| a0 | `3db0b4` | NPC child mouse dispatch |
| a1 | `abd428` | Held draw XY argument; original renderer always runs |
| a2 | `3cc97c` | Local W9 item-getter decision; no global F0 write |
| a3 | `abcca4` | Current physical Grid hover; otherwise full original clear/Leave |

## Layout preparation

`dm/ios/tools/pc_item_layouts.py` builds the Bank/Chronicle, inventory, vendor and hireling layouts from the user's own RomFS. Its `build(romfs,new_output_directory,prior=None)` returns a replacement dictionary and statistics. The PC overlay generator may import it optionally and merge that dictionary over its other replacements; no Spanish generator is required.

The preparation keeps shared-stash callbacks, adds the personal pager and sixth Chronicle tab, and lays out the native 91 advanced-stash slots. It preserves the upstream runeword button, Pouch/deposit controls, vendor refresh and six extra mercenary equipment slots. It produces six textures and two inventory tables locally; those tables widen the stash rows and retain the existing mercenary slot adjustments. The existing UI string-table overlay must be applied once after merging layouts, as the PC-input generator does. No generated JSON, textures or tables are committed. These are HD layouts; this change does not certify redesigned classic-SD panels.

## Verification and limits

With the usual prepared workspace and own game files:

```sh
make -C dm/host/tests pc-items
```

The target runs production C policy, the public snapshot/legacy-link boundary, Pouch/Vault fixtures, synthetic layout tests, and production-AST gate tests. The latter need Unicorn from the prepared Python environment. `DM_MAIN_ORIGINAL` may select the local original image for individual gate tests; it is never downloaded.

On the isolated slice the C tests reported 136 item-boundary checks, 294 tooltip-domain checks, 218 missing-profile checks, Pouch 14, and Vault 27/36/28/23, all without failures; UBSan repeated these successfully. The profile/skill-extension link check passed. Layout fixtures passed 10/10; preparing the own RomFS produced twelve nonempty replacements. The gate tests passed 40 item cases, A2 12 cases and A3 9 cases plus eight AST guards. The independent structural verifier accepted all seven stubs and rejected 110 single-word mutations. Gate tests execute the production stubs, with explicit host-dispatch doubles; they do not execute the native item operation or GPU renderer.

A disabled-profile negative control fails the positive behavior checks. Cancellation regressions were red before preserving the epoch fence and invalidating Vault capture identity, and green afterwards. The END→publisher epoch bump→fresh gesture adapter sequence is covered. Modifier tests execute the production orchestration with native API doubles; asynchronous acknowledgement tests cover the state transition and idempotency, not a full server exchange.

After generating the touch product in a new isolated temporary workspace, all three structural checks and full rewrite-equivalence regressions passed (25,084/11,975/10,486 equivalence sites, zero divergences, 900 mutations detected), as did the native gate harness (153/0). The added item sites are intentional control-flow cuts checked by the structural verifier and explicit gate/policy fixtures; stock equivalence does not prove their full native item-domain or server semantics. An independent combined-generator check also confirmed 25 resources (19 replacements, six additions), 16 resolvable @dm references and 2,131 unchanged native strings.

The string-table check and native-write/cache/mode scans passed. The repository's literal scan still reports two unchanged baseline diagnostics in `gen_romfs_overlay.py` and `drop_light.py`; neither file belongs to this slice. Full host/gfx/iOS suites and combined-input GUI acceptance have not been run for this branch. Tooltip/A3 adaptation covers inventory, bank and vendor domains; this slice does not add that adaptation to Hireling/Cube panels. Historical local QA of the combined port is not a substitute for checks on this proposed split.
