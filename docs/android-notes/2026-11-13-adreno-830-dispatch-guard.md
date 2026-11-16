# Adreno 830: compute dispatches of 8×4×1 run an extra row of work groups; compute shaders get a dispatch-range check (2026-11-13)

中文：[2026-11-13-adreno-830-dispatch-guard.zh-CN.md](2026-11-13-adreno-830-dispatch-guard.zh-CN.md)

[#256](https://github.com/jhaizhou-ops/d2r-mobile/issues/256): on Snapdragon 8 Elite phones (Adreno 830), lava-lit ground in the top right of the picture shows sharp rectangles of wrong lighting that flicker (Act IV River of Flame shows it well). [#261](https://github.com/jhaizhou-ops/d2r-mobile/issues/261) (Adreno 825) reports bars in the same place.

## What the GPU does

- The game builds a light list per screen tile with one compute shader (`b54d192f7c620ded`, local size 8×4×1, one invocation per tile), dispatched (4,5,1) for a 32×20 tile grid. Layer 0 of the lists is slot `tile`, layer 1 is slot `640 + tile`, in one buffer.
- On the Adreno 830 (OnePlus 13, driver `0x80320050`) a dispatch with local size exactly 8×4×1 and an odd y group count runs one extra row of work groups. A probe shader that counts every invocation shows it for y = 1, 3, 5, 7, 9, 25 and 27, and never for even counts or for any other local size tried (4×4, 4×8, 8×1, 8×2, 8×3, 8×5, 8×8, 16×4, 32×4, 32×1, 64×1, 8×1×8, 4×4×4, 8×8×8 ...). The Adreno 730 (Redmi K50 Ultra) and Mesa's llvmpipe run exactly what was dispatched.
- The extra invocations of the light-list dispatch act as tile rows 20–23. Their layer-0 slots (640–767) are the layer-1 slots of rows 0–3, so the top four rows of tiles get lights from elsewhere, and since lava lights change every frame, they flicker.
- Proof: the shader run alone on inputs taken from a RenderDoc capture gives wrong layer-1 lists for rows 0–3 in every run on the 830 (different every run) and correct lists on the 730 and on llvmpipe (byte-identical); running a real sixth row on llvmpipe reproduces the 830's data.

## Change

`xbg_spirv_dispatch.cpp`: when a compute module loads, the engine puts this first in its entry function:

```
if (any(gl_WorkGroupID >= gl_NumWorkGroups) || any(gl_LocalInvocationID >= local size) ||
    any(gl_WorkGroupID * local size + gl_LocalInvocationID >= gl_NumWorkGroups * local size)) return;
```

An invocation inside the range computes exactly what it did. On by default on Qualcomm Adreno 8xx (`xbg_adreno_dispatch_overruns`), marker file `gfx-dispatch-guard` (1 / 0) to force it, environment `DM_GFX_DISPATCH_GUARD` for other GPUs. The module loaders apply it after the other rewrites; the import pack is unchanged.

**Why the position comparison.** In the light-list shader the extra invocations have an in-range work group id and an out-of-range local id (y 4–7). The 830's compiler takes the local id to be below the local size and removes a comparison of the local id with the literal size, so the first version of the check (the first two terms alone) changed nothing. Comparisons with limits read from a buffer, and the position comparison above, are kept.

**Risk.** The position comparison stays with today's driver. A later driver could reason it away as well (the local id is below the size, the work group id below the count), and the flicker would come back without anything failing. A check whose limit is read from a buffer cannot be removed; for every compute shader that needs the dispatch size passed in (a push constant), which changes how compute pipeline layouts are made (layout keys, warm-up list). Left until it is needed. After a driver update on an Adreno 8xx phone, look at the River of Flame waypoint.

## Verification

- Unit tests: `g++ -std=c++17 -Wall -Wextra dm/gfx/vulkan/engine/tests/dispatch_guard_test.cpp dm/gfx/vulkan/engine/xbg_spirv_dispatch.cpp -o /tmp/dispatch_guard_test && /tmp/dispatch_guard_test` (96 module shapes, every invocation inside / outside the range, the 830's extra invocations with the local id comparison removed, 18 forms left untouched), and the additions to `adreno_policy_test.cpp`. Both also pass under AddressSanitizer and UBSan.
- All 26 compute modules of the import pack take the check through the engine's load chain (warp32 at 64 and 128 lanes, NaN min/max, component fold, forwarded temporaries) and pass `spirv-val` (Vulkan 1.1); 6,905 modules of other stages are untouched.
- The light-list shader alone on the captured inputs, OnePlus 13:

| Shader | Runs | Right |
|---|---|---|
| unchanged | 5 | 0 |
| first two terms only | 20 | 0 |
| with the position comparison | 40 | 40, byte-identical to the 730 and llvmpipe |

- In game on the OnePlus 13: the guarded shader as an `spv-override` first, then the debug build: no rectangles at the River of Flame waypoint; the log shows `dispatch_guards=13` and no compute module left unguarded; 60 fps.
