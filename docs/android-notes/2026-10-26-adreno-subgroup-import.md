# Adreno 750: opt-in subgroup-helper inlining for import packs (2026-10-26)

中文：[2026-10-26-adreno-subgroup-import.zh-CN.md](2026-10-26-adreno-subgroup-import.zh-CN.md)

This adds an **opt-in import-generation path**, not a new default for every GPU. It follows the helper-inlining suggestion in [#123](https://github.com/jhaizhou-ops/d2r-mobile/issues/123), after a Y700 test reproduced the startup compiler crash with [#148](https://github.com/jhaizhou-ops/d2r-mobile/pull/148)'s two rewrites already off.

## What was observed

Device: Lenovo TB321FU / SM8650 / Adreno (TM) 750, Android 16, Vulkan 1.3.128, driver `0x802fa028` (0762.40), 4096-byte system pages. Original test source: `445bf05e600dabb735f89d932469396f6a15ea1a` (PR148), PRODUCT `3.3.94012-r44`.

| Import pack, same native APK | Result on this device |
|---|---|
| Default, runtime quad=0 and warp32=0 | 3 startup failures before character selection, SIG11 / addr=0x48 / exit139 |
| Existing `DM_VK_INLINE_CS=1`, five compute modules | Passed the original compute shader, then failed at a pixel shader in the same driver compiler; two failures |
| All stages with non-entry subgroup helpers inlined | Three boots reached character selection; character creation, camp movement, outdoor/cave rendering, combat, save/exit and restart/load were observed |

Process maps placed the baseline PC in `libllvm-qgl.so+0x463ed4`, LR at `+0x2ec048`. That is evidence for this driver/compiler path, not proof that every Snapdragon startup failure has the same cause. Credit for identifying non-entry subgroup helpers and proposing the inlining passes belongs to the reporter of #123.

The first successful process remained alive for at least 31 minutes, including at least 18 minutes of camp observation. Blood Moor and Den of Evil were reached with the project's temporary `DM_TEST_GOTO_LEVEL=100:2;2000:8` switch, rather than normal travel. Combat reached level-1 XP 21%; the unattended test character subsequently died, continued to camp, recovered equipment and saved. The temporary env was removed before the third boot, which loaded the save normally. No application data was cleared.

## Change

Set `DM_VK_INLINE_SUBGROUP=1` when generating an import pack. For modules with `OpGroupNonUniform*` in non-entry functions, the exporter runs:

```
spirv-opt --target-env=vulkan1.1 --merge-return --inline-entry-points-exhaustive --eliminate-dead-functions
```

It runs after binding/capability handling, before final validation and reflection. A failed optimizer or remaining subgroup helper fails the build. The ordinary Vulkan 1.1 validator still checks every module. In the tested pack, 187 game modules change: 182 pixel and five compute. Modules without such helpers are left untouched. The native APK, descriptors and XGS layout are unchanged.

The new switch takes precedence over the existing compute-only switch. With neither switch, or with only `DM_VK_INLINE_CS=1`, the previous behavior is retained. `pack_info.txt` records `SHADER_SUBGROUP_INLINE=1` when the new mode is used.

## Use only with both runtime rewrites off

The pixel quad rewriter matches the translator's original helper-call shape. Offline inlining can remove that shape, so **do not use this pack on a device that needs quad/warp rewrites enabled**. PR148 defaults both off on Qualcomm Adreno 740–799, but explicit marker files still override that policy. Verify both are 0 in the device log. Adreno 740 has not been tested with this import mode; neither have 730, 735, 830, other vendors or other driver versions.

From source compatible with the installed APK:

```sh
DM_VK_INLINE_SUBGROUP=1 python3 dm/tools/d2rm_android.py <Update-folder> <new-output-folder>
```

Import the resulting `android` folder through the normal application flow. It contains your own game content and must remain private. No extra dependency beyond the packer's existing SPIRV-Tools is introduced. No APK or game package is included in this change.

Current main also has `81e692d`'s compute-pipeline skip/failure protection ([#153](https://github.com/jhaizhou-ops/d2r-mobile/issues/153)). This change preserves that protection, including its Adreno750 skip; it does not re-enable the skipped fluid shader. The Y700 native observations above used the earlier PR148 APK and do **not** establish whether the latest default APK still fails. This mode provides a reproducible additional workaround for the pixel-helper failure and a way to generate the already-tested shader form.

## Validation and limits

- Source literal and i18n checks passed before and after the change.
- `python3 dm/gfx/tools/tests/test_dmsh_vk_inline.py`: hand-authored compute/fragment SPIR-V exercises real optimization and validation, no-op preservation and fail-closed behavior. It needs the same `SPIRV_OPT` / `SPIRV_VAL` tools as the packer; no game files.
- Real full-pack regeneration and comparison against the device-tested artifacts are recorded in the pull request.
- The original device test used fresh pipeline caches archived after normal save/exit; saves were retained. Generated shaders, screenshots, game files and raw device logs remain private.
- Long play, full-game completion, subjective audio, reference-image equivalence, other GPUs/drivers and a physical 16KB-page device remain unverified. Static APK/ELF 16KB alignment is a separate check.
