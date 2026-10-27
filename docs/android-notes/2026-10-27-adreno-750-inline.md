# Adreno 750: shaders with subgroup instructions in helper functions are inlined at load; the pipeline guard covers graphics pipelines (2026-10-27)

中文：[2026-10-27-adreno-750-inline.zh-CN.md](2026-10-27-adreno-750-inline.zh-CN.md)

[#123](https://github.com/jhaizhou-ops/d2r-mobile/issues/123), [#153](https://github.com/jhaizhou-ops/d2r-mobile/issues/153), [#166](https://github.com/jhaizhou-ops/d2r-mobile/issues/166) and one private report (Samsung, driver `0x802fa029`) are the same thing: on the Snapdragon 8 Gen 3 (Adreno 750), v2.0.1 still exits around the main menu.

## What the phone logs show

- Every crash is inside Qualcomm's own shader compiler, `libllvm-qgl.so`: `sig=11 addr=0x48`, return address at offset `0x2ec048` in the library, crash at `0x463e84` (Xiaomi 14, Redmi K80) or `0x463ed4` (Samsung; another driver version, the same function).
- v2.0.1 skips only the compute shader `098a126f92bf533a`. In #166, once it was skipped, the next compute shader `6a7a308d61928982` crashed the same way, and so did three pixel shaders of 30,000+ words; with their helpers inlined, the game reached camp.
- The Samsung log covers three boots of 45-47 shaders each. The pixel shaders whose subgroup instructions sit in a called helper (`358197ac3db59cbc`, `5c1ca4e7c3e4657c`) were loaded 3 times and crashed all 3 times while the graphics pipeline was built synchronously; the other 42-44 all compiled, including a 36,515-word pixel shader without that shape. The trigger is the shape, not the size.
- The pack has 187 shaders of that shape (5 compute, 182 pixel), and all 8 that crashed are among them.

## Changes

1. **Inlining at load in the engine** (`inline_rewrite` in `xbg_game.cpp`): on Qualcomm Adreno 750-799, a shader whose subgroup instructions sit in a called helper has its helpers inlined into the entry point with the SPIRV-Tools passes merge-return, inline-entry-points-exhaustive and eliminate-dead-functions before it goes to the driver. The import pack is unchanged; players only install the new APK. Other phones keep the original shaders (the pixel shaders' quad rewriter matches the helper-call form). The marker file `files/dm/gfx-inline` (`1` / `0`) turns it on or off explicitly; Adreno 740 keeps only the rewrites off and does not inline. The boot log gains `NVN shaders with subgroup helpers are inlined at load=…`, and each shader's `loaded` line gains `inlined=`.
2. **SPIRV-Tools in the repository** (`dm/third_party/spirv-tools`, v2026.3, library sources and pre-generated tables only, see its README): built into the Android package only; the engine on other platforms such as HarmonyOS has no such step. The library is compiled without debug info (the Android package keeps the .so debug info, which would take the APK past 200 MB); the APK goes from 23 MB to 34 MB. The experimental `DM_VK_INLINE_CS` switch is removed.
3. **The pipeline guard covers graphics pipelines**: it used to cover compute pipelines only, with a single marker file. Now all three paths that build pipelines (the game's graphics thread, the warm-up threads, the async thread) write the pipeline to their own thread's `gfx-pipe-inflight.<slot>` before the build and remove it afterwards. An entry goes into `gfx-pipe-skip` only when it was in flight on two boots in a row; a driver error is recorded on the spot. Entries are `c:` plus the shader key for compute pipelines, and `g:` plus the stage keys for graphics pipelines. Entries recorded by v2.0.1 (no prefix, recorded without inlining) are dropped when read, with the log line `NVN pipelines skipped by an older version dropped=…`; if a shader still crashes it is recorded again. The v2.0.1 rule that skipped `098a126f92bf533a` outright on the 750 is removed: inlined, it builds (checked on an S24 in #123), so the fluid effect comes back; should it still crash, the guard catches it. Only draws with that exact combination are skipped, so other draws sharing the vertex shader still run. A graphics pipeline error no longer makes the whole app exit.
4. **Logs**: on Android, the graphics engine's lines move from the tag `dm-vulkan` into `dm` and into the log file, so `adb logcat -s dm` gets everything. In the crash line, a library without symbols is printed as name+offset (for example `libllvm-qgl.so+0x463e84`), which compares directly across phones.

## Verification

- Load-time inlining matches the offline tool: inlining the pack's 187 shaders of that shape with the library built from `dm/third_party/spirv-tools` gives the same bytes as `spirv-opt --merge-return --inline-entry-points-exhaustive --eliminate-dead-functions` (187/187). The import pack is unchanged (`dmsh_vk.py` only loses the experimental switch).
- Unit test: `g++ -std=c++17 -Wall -Wextra dm/gfx/vulkan/engine/tests/adreno_policy_test.cpp -o /tmp/adreno_policy_test && /tmp/adreno_policy_test`.
- On a phone (vivo, Snapdragon 8 Elite / Adreno 830, driver `0x80320049`, debug build, an import pack identical to one made with v2.0.1): there is no 750 at hand, so this checks that the inlined shaders compile on Qualcomm's compiler and render the same, that the guard's record / skip / restore / old-entry paths work, and that the 830 default is unchanged. That the 750 no longer crashes needs the reporters.

| Run | Setup | Expected | Result |
|---|---|---|---|
| A | default | no inlining, in game, no crash | in game, 60 fps, no crash; none of 97 shaders inlined |
| B | `gfx-inline=1` | only loaded shaders among the 187 inlined, in game, no crash | 97-98 loaded; all 10 of them among the 187 inlined, 0 others; in game, 60 fps, no crash; camp screenshot vs A: mean difference 1.19, 99th percentile 19 |
| F | `gfx-inline=1`, `gfx-shader-reuse=2` | the warm-up's and the graphics thread's inlining identical word for word | 0 mismatches |
| G | v2.0.1's skip list (`098a`, `6a7a`) plus `gfx-inline=1` | old entries dropped, both shaders back | `dropped=2`; both inlined and dispatched; no crash |
| C | two boots in flight on `g:54f730a6c0efc118+358197ac3db59cbc` | recorded at boot, skipped when the main menu draws it, no crash | as expected; in game, 60 fps |
| E | only the last boot | suspect only, not skipped | only `gfx-pipe-suspect` |
| D | everything removed | restored | as expected |

Load cost: shaders the warm-up already handled take 0.1-0.3 ms on the graphics thread; the two compute shaders, which the warm-up list does not cover, are inlined on their first load each boot (on the 830, `6a7a308d61928982` about 30 ms and `098a126f92bf533a` about 8 ms, while the main menu loads).
