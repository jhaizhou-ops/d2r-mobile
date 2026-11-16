# Drivers without fragment-stage subgroups: fragment shuffles rewritten in relaxed mode, check after the rewrites (2026-11-16)

中文：[2026-11-16-fragment-subgroup-fallback.zh-CN.md](2026-11-16-fragment-subgroup-fallback.zh-CN.md)

[#243](https://github.com/jhaizhou-ops/d2r-mobile/issues/243) and [#155](https://github.com/jhaizhou-ops/d2r-mobile/issues/155): the game aborts at the main menu with `Actual subgroup operation unsupported in shader stage` on drivers whose `VkPhysicalDeviceSubgroupProperties::supportedStages` holds only compute (`NVN caps subgroup ... stages=0x20`). Seen on old Adreno 650 drivers (Snapdragon 865 / 870: Retroid Pocket 5, Realme GT Neo 2; Vulkan 1.1.128) and on the Maleoon 910 (Pura 70 Ultra, Mate X5). The analysis is @kieusonlam's (#155).

## What the shaders contain

A survey of the import pack (4,559 fragment shaders) with the engine's own rewrite code:
- 182 fragment shaders use subgroup instructions, each through one translator helper (SHFL.BFLY: `OpGroupNonUniformShuffle` on a float, with `SubgroupLocalInvocationId`). Vertex shaders use none; 5 compute shaders do, which these drivers allow.
- The existing quad rewrite (`gfx-quad-derivatives`, on by default on Android outside the Adreno 740–799 family) replaces the helper *calls* with fine derivatives: 271 of 278.
- After it, 176 shaders no longer reach a shuffle, but the uncalled helper, the lane built-in and the capabilities stay in the module. 6 shaders still call it: 5 calls whose result also has bit uses, and one shader whose 2 calls pass a value the conservative analysis can't prove numeric (an `FAdd` of a value loaded through a computed index).
- `validate_capabilities` ran in the runtime loader before any rewrite.

## Change

- `xbg_spirv_quad_derivatives(..., relaxed)`: only on drivers without fragment subgroups.
  - It also rewrites calls whose result has bit uses, or whose value's immediate producers are float arithmetic or float loads (as floats).
  - It rewrites calls whose value comes from `OpConvertFToS` / `OpConvertFToU` (back to float, neighbour, back to integer; exact for |v| < 2^24). D2R has none, but the unit test covers it.
  - Divergent control flow and other sources stay skipped. A direct shuffle in the entry function keeps no shuffle fallback.
- `xbg_spirv_strip_subgroup`: removes functions the entry point cannot reach, subgroup built-in input variables nothing reads (annotations and interface entries included), and `GroupNonUniform*` capabilities once no subgroup instruction is left. It is self-contained, so the HarmonyOS build gets it too.
- `quad_rewrite` applies both on such drivers whatever `gfx-quad-derivatives` says. The runtime loader checks capabilities after the rewrites, and `DerivativeControl` is accepted.
- Test switch: marker `gfx-no-fragment-subgroups` (1) or `DM_GFX_NO_FRAGMENT_SUBGROUPS=1` clears the fragment bit.

## Verification

- **Unit test** `dm/gfx/vulkan/engine/tests/spirv_quad_test.cpp`: 52 cases, 17 of them new.
  - Relaxed mode: proven calls come out word for word as in the default mode; bit-use and integer-source calls give the neighbour's exact value; calls under divergent control flow are still skipped.
  - Clean-up: the helper, the lane built-in (with annotations and interface entry) and the capabilities go, with results unchanged; nothing is removed while the helper is still called.
  - It passes with clang 18 and under ASan + UBSan with gcc 14. The emitted fixtures pass spirv-val, apart from the existing wrong-scope rejection fixture.
  - Command: `clang++ -std=c++17 -UNDEBUG -I dm/gfx/vulkan/engine dm/gfx/vulkan/engine/tests/spirv_quad_test.cpp dm/gfx/vulkan/engine/xbg_spirv_quad.cpp -o /tmp/quad_test && /tmp/quad_test`
- **The whole import pack, offline**, through the engine's own rewrite code (game content, so local only):
  - Relaxed mode plus clean-up: all 182 fragment shaders with subgroup instructions end with none, no lane built-in and no `GroupNonUniform*` capability. 278 calls are rewritten: 271 as today, 7 relaxed as floats. All 182 pass `spirv-val --target-env vulkan1.1`.
  - The default mode is unchanged: the 176 modules it rewrites are byte-identical to main's output.
  - The moved check against every family's load-time chain, 6,931 shaders each:
    - Android default (quad, warp32, NaN min/max, constant fold, forwarded temporaries);
    - Adreno 750–799 (SPIRV-Tools inlining of the 187 modules with subgroup helpers, then tidy);
    - Adreno 740 (tidy);
    - relaxed;
    - HarmonyOS without the Android switches.

    0 shaders fail it in any of them.
- **Redmi K50 Ultra (Adreno 730), debug APK from this branch**, 3D scale held at 0.75, fixed game seed:
  - Camp, switch off: the normal path (`fragment subgroups=1 stages=0x30`).
  - Camp, switch on (`stages=0x20`): no abort. The shuffle shaders load with `stripped=4`, including the relaxed one (`00f1902d880682c7`, `relaxed_float=1`). Both runs: warm-up 392 built, 0 failed, 178 warm hits, 0 pipelines created during play. The screenshots differ only in animated content (0.2% of pixels by more than 24/255). GPU 14.23 vs 14.17 ms (separate runs).
  - Den of Evil (engine warp), switch on: no abort, 118 shaders loaded through the check during play, 189 warm hits, 0 created during play, and the cave and its lighting render normally.
- Full build: debug APK built in docker (NDK 29), no warnings.

## Not covered

- No Adreno 650 or Maleoon 910 here; the Redmi only fakes the missing feature.
- Adreno 650: the fluid compute shader `098a126f92bf533a` still fails to build there (`VK_ERROR_UNKNOWN`, #155), so smoke and fire show garbage blocks. Separate change.
- Maleoon 910: #249 reports a driver crash while drawing characters once the check is passed. Not addressed here.

## Follow-up (2026-11-17): the inlining order on a real Adreno 650

@kieusonlam tested #280 on a Retroid Pocket 5 (stock driver, `stages=0x20`, #243). Neither `gfx-inline` setting booted:
- With `gfx-inline=1`, the helpers were inlined before the quad rewrite. The shuffles then sat in the entry function under divergent control flow, the rewrite replaced none of them, and the check aborted.
- With `gfx-inline=0`, the fluid compute shader `098a126f92bf533a` crashed `libllvm-qgl.so` (addr 0x48), the Adreno 750 family's compiler crash.

Change:
- `front_rewrites`: on drivers without fragment subgroups, the quad rewrite and clean-up run before the inlining. With fragment subgroups the order is unchanged.
- `xbg_adreno_inline_default`: inlining is on by default for the 750–799 family and for Qualcomm drivers without fragment subgroups. The marker `gfx-inline` still wins.

Verification:
- `tests/adreno_policy_test.cpp` passes with 7 new cases.
- Offline, the Adreno 650 chain (fragment: relaxed quad + clean-up, then inlining; compute: inlining, warp32, tidy) fails the check on 0 of 6,931 shaders. No fragment shader still needs inlining, and all 187 modules with subgroup use pass spirv-val, including the 5 compute shaders that are inlined and then go through warp32.
- Redmi (Adreno 730), camp, switch on, so inlining is on by default: no abort. 17 fragment shaders were rewritten before inlining (`stripped=4`), none of them inlined; `098a126f92bf533a` and `6a7a308d61928982` were inlined, then warp32. Warm-up 392 built, 0 failed, 178 hits, 0 created during play, GPU 14.34 ms. With the switch off: inlining stays off as before, 14.38 ms. The screenshots differ only in animated content.
