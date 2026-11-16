# Samsung Xclipse: no D24 depth formats; 24-bit depth is stored as 32-bit float (2026-11-16)

中文：[2026-11-16-xclipse-depth24-float.zh-CN.md](2026-11-16-xclipse-depth24-float.zh-CN.md)

[#250](https://github.com/jhaizhou-ops/d2r-mobile/issues/250): on Samsung Exynos phones (S22 Ultra / Xclipse 920, A55 / Xclipse 530, S25 FE / Xclipse 940) the game shows a black screen for a few seconds and aborts with `vkGetPhysicalDeviceImageFormatProperties(...) failed: -11` in `get_image_slow` ← `clear_attachment`.

## Cause

- The game's depth buffers are NVN Z24S8 (0x35) and Z24X8 (0x33). The engine created them as `VK_FORMAT_D24_UNORM_S8_UINT` and `VK_FORMAT_X8_D24_UNORM_PACK32`.
- Samsung's Xclipse driver (AMD RDNA) has no features for either: the A55 log's `NVN caps formats` lines skip formats 125 and 129, and the S25 FE's Vulkan Caps Viewer shows "Optimal tiling: none" for both. Vulkan only guarantees D24S8 or D32S8 for depth-stencil attachments, and these drivers have `D32_SFLOAT` (126) and `D32_SFLOAT_S8_UINT` (130): attachment, sampled, depth compare, transfer (A55: `0xce01` and `0xc601`).
- The same drivers report no `SAMPLED_IMAGE_FILTER_LINEAR` for any depth format (D16 included), so the engine's samplers already fall back to NEAREST on depth there, with an `NVN sampler linear unsupported` line.

## Change

- `choose_depth_storage` (`xbg_game.cpp`, run before the warm-up threads start): when X8_D24 or D24S8 lacks depth-stencil attachment, sampled image or transfer features and D32F / D32FS8 have them, Z24X8 is stored as `D32_SFLOAT` and Z24S8 as `D32_SFLOAT_S8_UINT`. Marker file `gfx-depth24-float` (1 / 0) or `DM_GFX_DEPTH24_FLOAT` forces it either way. Start-up line: `NVN depth24 Z24X8/Z24S8 stored as ...`. The D16 choice (`depth16_as_d24`) moved into the same function unchanged.
- Shaders read the same 0..1 depth from either format.
- Uploads (`append_depth_planes`) turn each 24-bit unorm value v into the float nearest v/(2^24−1); readbacks (`copy_texture_buffer`) turn the float back into the nearest unorm value (NaN and negatives → 0, above 1 → 2^24−1). `xbg_depth_unorm24_to_float` / `xbg_depth_float_to_unorm24` in `xbg_depth.c`; the round trip is exact for all 2^24 values.
- Warm-up list: render pass attachment formats in loaded records are remapped the same way (D24S8 → D32FS8; X8_D24 → D32F unless X8_D24 is also the D16 stand-in), so seed records made on other phones match the runtime keys and no render pass is built with a format the driver lacks. `NVN warm list=... depth24-remapped=N` counts them.
- Depth bias: a float depth buffer's constant bias unit follows the exponent of the primitive's depth (2^-24, the same as D24, for depths in [0.5, 1), finer below). The engine leaves the constant as it is and logs the first four biased draws on a stand-in (`NVN depth bias on the float stand-in ...`).

## Verification

- **Unit test** `dm/gfx/vulkan/engine/tests/depth24_float_test.c` (CPU only): all 2^24 values round-trip exactly, map to the nearest float and increase strictly; float → unorm edges (NaN, ±0, negatives, infinities, denormals, 1 − 2^-24) and nearest rounding on every 4099th float in [0, 1]. 67,628,688 checks, 0 failures with gcc 14 `-O2`, gcc 14 `-O0 -fsanitize=undefined` and clang 18 `-O3`:
  `cc -std=c11 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/depth24_float_test.c dm/gfx/vulkan/engine/xbg_depth.c -lm -o /tmp/depth24_float_test && /tmp/depth24_float_test`
- **Redmi K50 Ultra (Adreno 730, which has D24), debug APK from this branch.** Rogue Encampment, 3D scale held at 0.75, fixed game seed, one cold start with marker `gfx-depth24-float` = 0 and one with 1:
  - Start-up with 1: `stored as D32_SFLOAT/D32_SFLOAT_S8_UINT (marker gfx-depth24-float)` and `NVN warm list=392 enabled=1 depth24-remapped=267`.
  - Warm-up in both runs: 392 built, 0 failed, 178 warm hits, 0 pipelines created at draw time. So the remapped records match the runtime keys exactly.
  - The two Z24S8 targets (2000×900 and 1500×675) were created as `D32_SFLOAT_S8_UINT` and their initial uploads went through the conversion. The D16 shadow maps stay X8_D24.
  - The scene depth (Z24S8) is sampled with a linear min filter, which D32FS8 doesn't support on the 730 either, so it drops to NEAREST, as it will on Xclipse.
  - There was no depth bias on the stand-ins (the camp uses bias only on the D16 shadow maps) and no depth readback, so the readback conversion is covered by the unit test only.
  - Screenshots of the same scene differ only in animated content (flames, rain, foliage, the chicken, the orbs, minimap labels). 0.3% of pixels differ by more than 24/255; walls, characters and shadow edges match.
  - Camp GPU time was 14.30 ms with D24 and 14.2 ms with the D32 stand-ins. These are separate runs, not an in-process A/B, so they show no visible cost rather than a measured difference.
- Full build: debug APK built in docker (NDK 29), no warnings.

## Not covered

- Needs a test on an Xclipse phone (the #250 reporters will be asked).
- Shadows on Xclipse: the game samples its D16 shadow maps (and the scene depth) with linear filtering plus depth compare, and Xclipse can linearly filter no depth format. The existing sampler fallback switches those samplers to NEAREST, so expect hard, stair-stepped shadow edges where Snapdragon shows soft ones. The Redmi runs don't show this for shadow maps: there D16 uses the X8_D24 stand-in, which filters linearly. If it looks too rough, the follow-up is filtering in the shader (several point-sampled compares blended). Keep #250 open until a reporter has checked shadows too.
- If the reporters' logs show depth bias on the stand-ins and shadow acne or z-fighting, `VK_EXT_depth_bias_control` (where offered) can request the D24 unit exactly.
