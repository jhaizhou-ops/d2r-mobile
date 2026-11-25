# tvOS memory optimization and scene-transition observations (2026-10-18)

**English** | [简体中文](qa-tvos-memory.zh-CN.md)

Two optimization rounds extended the observed play time, and a short balanced-policy run recovered close to 60 FPS. **Town returns, re-entering caves and quick restart are still not consistently stable.** This is evidence for reviewing an experimental patch, not stability acceptance. Only sanitized numbers and host-log excerpts are included; complete device logs remain local.

## Environment and accounting

- Apple TV 4K, third generation, A15, 4 GB, tvOS 27.0; Legacy Device ASTC Pack deployed, dynamic resolution retained, initial scale 0.75.
- Mac M4, macOS 27.0.1, Xcode 27.1; game version 3.3.94012.
- Device observations came from local development builds based on `1784563`. Before submission, the patch was integrated with upstream `9393b00` and host/targeted graphics tests were rerun. The final integrated revision has not completed a fresh device regression.
- Footprint, the kernel-recorded peak and `os_proc_available_memory()` headroom are in MiB. Metal allocation, guest heap capacity and process footprint have different accounting and must not be added together. Physical device RAM is not the app's memory allowance.

## Round one: aggressive read-only upload-cache eviction

The first experiment checked every 60 frames, protected textures used in the last 120 frames and used a 384 MiB soft budget. Below 256 MiB headroom it attempted at least 64 MiB of eviction, with a 128 MiB batch target cap. GPU-written textures were protected; the existing GPU fence retained in-flight objects. Eviction also cleared descriptor aliases and invalidated argument-buffer caches to avoid stale GPU handles.

The player reported longer play without another exit during that test, but noticeable stutter. Logs showed repeated small trims:

```text
tvOS 贴图缓存：回收 6 张 26 MB（等待 GPU fence），缓存 205 → 179 MB，系统余量 173 MB
tvOS 贴图缓存：回收 2 张 0 MB（等待 GPU fence），缓存 181 → 180 MB，系统余量 193 MB
```

These mean 26 MiB reclaimed, followed by a sub-MiB trim; integer MiB logging can display zero. Some runs also enabled `DM_RES_CHECK` and residency census and reached thermal state 2. Diagnostic locking on the texture-binding hot path affects performance. The entire frame-rate regression cannot be attributed to eviction, nor compared as a controlled A/B against a later cool-device short run.

## Round two: preserve ASTC and reduce eviction churn

The final patch uses this balanced policy:

| Policy | Normal | Critical (<96 MiB headroom) |
|---|---|---|
| Minimum scan interval | 3 seconds | 1 second |
| Recently used protection | 600 frames | 120 frames |
| Batch target / target cap | At least 16 under pressure / at most 32 MiB | 64 / 64 MiB |
| Small batches | Skip when cold data is below 4 MiB | Do not skip |

Pressure starts below 160 MiB and ends at 224 MiB; the soft budget remains 384 MiB. A texture is indivisible, so the last eviction can overshoot the target. Active and GPU-written resources are protected, and the cache can exceed its soft budget. This is not a process-memory cap guarantee.

For an initial copy into a format/block-compatible BC3 2D/array destination, retain ASTC blocks from the pack. A later incompatible copy, buffer write, clear or attachment write preserves existing mip/slice contents before falling back to RGBA. One compressed allocation of `128x128`, 42 layers and 1344 KiB was observed. There is no demonstrated large whole-process peak reduction attributable to this path, and fallback coverage on devices remains incomplete.

A short run with `DM_RES_CHECK` and residency census unset recorded:

| Seconds after launch | Thermal state | 3D scale | Presented FPS |
|---|---|---|---|
| 10 | 0 | 0.75 | 58.9 |
| 20 | 0 | 0.75 | 59.9 |
| 30 | 0 | 0.75 | 59.9 |

This window does not establish sustained frame rate or transition stability. `DM_TV_TEXTURE_CACHE_MB=0` disables eviction; `DM_TV_ASTC_ARRAYS=0` disables ASTC preservation for future controlled comparisons.

## Town return, quick restart and cave re-entry

A separate host-retention defect was found: `TextureFinalize` removed only the serial entry, leaving cached descriptor textures/views alive until those slots were registered again. The patch retires every matching alias, preserves the guest descriptor snapshot, invalidates argument buffers and retains in-flight objects through the existing GPU fence. This changes the shared Metal backend; tvOS observations do not replace other-platform regression checks.

| Case / stage | Evidence | Interpretation |
|---|---|---|
| Original build, approximately 6.5 minutes of play | System Jetsam report: `DmTV`, `reason=per-process-limit`, `rpages=134446`, 16384-byte pages, approximately 2100.72 MiB of resident pages | This exit is confirmed to hit a process limit; resident-page accounting differs from footprint |
| Balanced policy, quick restart before alias fix | Native save/exit, frontend load, loading screen at 202 ms, then `signal 9`; last low-frequency sample 1991 MiB, headroom 106 MiB, thermal 0 | Exit reproduced; no matching new Jetsam report, final termination cause unconfirmed |
| Quick restart after alias fix | Entered the game in 1919 ms and verified the same character | One pass; a later run still exited with `signal 9`, last low-frequency sample 1932 MiB / 165 MiB headroom |
| Cave → town, alias fix and tracing enabled | Host area 8 → 1; player confirmed success; snapshot of 2644 samples had a 2074 MiB kernel-recorded peak and 46 MiB minimum sampled headroom | One pass, still little memory margin |
| Same observation run, town → cave | Player reported another exit; host area 1 → 8 followed by `signal 9`; 6015 samples recovered, last at 622668 ms: footprint 2054 MiB, kernel-recorded peak 2086 MiB, headroom 43 MiB | Additional failing direction; still no matching new Jetsam report, so signal 9 alone does not establish cause |

Consecutive samples just before the latest exit, with sampler column names and MiB rounded down:

```text
elapsed_ms  footprint_MiB  peak_MiB  headroom_MiB  GPU_MiB
622462      1963           2074      134           2076
622568      2035           2074      62            2135
622668      2054           2086      43            2185
```

Headroom fell quickly during loading. Approximately 100 ms sampling can still miss shorter footprint peaks. Thermal state 2 also occurred in this run; neither thermal state nor the final memory sample alone proves the termination cause. The original Jetsam confirms one memory-limit exit, while subsequent exits still need matching system reports.

## Observation and validation

Temporarily set `DM_TV_MEM_TRACE=1` to write memory to device `Caches/D2R/memory-trace.tsv` approximately every 100 ms, up to 18000 samples. Each sample is flushed; the next enabled launch overwrites the file, so retrieve it before relaunching after an exit. `DM_RES_CENSUS=1` additionally enables large texture-format groups. Leave both and `DM_RES_CHECK` unset for normal performance measurements.

Completed: full `make -C dm/host/tests run`, including `t_pkg_cache`; under Metal API Validation, `t_tex_finalize` passed 11 checks, `t_rt_srgb_view` 14 and `t_nvn` 75, all with zero failures; text checks passed. Signed tvOS builds succeeded during development. Full host and these targeted graphics tests passed again after upstream integration.

The full `make -C dm/gfx/tests run` stopped at 2 failures out of 85 in local `t_refl` binding-reflection expectations. That test does not link the modified `nvn_mtl.m`; no full graphics-suite pass is claimed. iOS builds/tests, rewrite-equivalence regression and Mac smoke were not rerun in this round.

Remaining acceptance: repeated bidirectional cave travel, consecutive quick restarts, long combat sessions, mixed ASTC/ordinary BC writes with fallback image checks, shared lifetime checks on other platforms, and diagnostic-free performance comparisons under matching scenes/thermal conditions. Keep this patch in draft.
