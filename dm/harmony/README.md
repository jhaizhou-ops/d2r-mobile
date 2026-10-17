# HarmonyOS source port

中文：[README.zh-CN.md](README.zh-CN.md).

This is an isolated source import of the existing HarmonyOS experiment, based on game **3.3.94012-r32**. It does not wire the current `dm/host` into HarmonyOS. The old `sw_*` touch/service ABI and the NVN object definitions are kept under this directory; do not replace them with current `dm_*` definitions without an ABI review. The PR base is `17845634d71341cca1debf43ad79ac25c6a3a97a`.

- `runtime/`: maintained HLE C services, ARM64 gates, loader, rewrite tools and earlier offline tests. All 17 current HLE service files are source inputs; they are not regenerated over the maintained versions.
- `graphics/`: Vulkan/NVN backend including the latest local descriptor changes after historical commit `334f556`. Paths point at the isolated `compat-r32/gfx/` snapshot.
- `app/`: ArkTS and NAPI source template, version **0.12.0 / 61**, unsigned products, plain development SVG icons. The signed shipping project and game art are not included.
- `compat-r32/`: only the old NVN interface/layout code and the five existing offline startup patch rules used by this rewriter. These are interface facts, not a copy of the game program. This adds another platform implementation of addresses/interfaces described in `docs/DERIVED.md`; maintainer discussion is still required.
- `tools/`: prepare external execution images, generate local table inputs, assemble an external unsigned DevEco project.

No original/rewritten game image, original shader, converted game shader, game art, skill table, experience table, save, key, certificate, compiled library or package is included. `gpu_assets.h` and `xbg_clear_spv.h` contain only this project's independently authored synthetic probe/clear shaders; their source/generators are retained. The preset shared-stash byte array was removed; the existing optional include in cloud sync falls back to normal original-game empty-stash behavior.

## Prepare locally

Use your legally owned matching `main`, `sdk` and RomFS. This import requires the official OHOS ARM64 native SDK compatible with **26.0.0**, DevEco Studio, Python with `capstone` and the upstream asset-generator dependencies, plus an OHOS ARM64 build of **Opus 1.5.2** with its license. SDK/API 26.0.0 is a build configuration, not a claim that a phone's HarmonyOS 7 system version equals that API.

The following commands run from the repository root. `$WORK`, `$NSO_MAIN`, `$NSO_SDK`, `$ROMFS`, `$NATIVE_SDK`, `$OPUS_LIB` and `$OPUS_INCLUDE` are your local paths. Use a new ASCII directory outside this checkout for `$WORK`; no credentials belong in these arguments.

```sh
python dm/harmony/tools/prepare_execution.py \
  --main "$NSO_MAIN" --sdk "$NSO_SDK" --output "$WORK/execution" \
  --native-sdk "$NATIVE_SDK" --opus-library "$OPUS_LIB" --opus-include "$OPUS_INCLUDE"
```

Omit the final three options for generation only. The tool checks the pinned NSO hashes and each decompressed segment hash, generates external images/assembly, applies the historical widescreen relocations, 512 MB VRAM budget and 1024 shadow-size cuts with original-word guards, then optionally builds `xblr_execution`. It deliberately does not run the old service generator, so the seven Claude-edited HLE files (audio, codec, fs, oe, svc, swkbd, thread) are retained. It does not build against the current `dm/host`. Do not use `python -O`: the historical rewrite code uses assertions as guards. This reorganized full game-generation/build chain has **not been run in this submission**.

Generate the external native skill index and PNGs through the existing upstream native-art generator, keeping the old Harmony ABI names:

```sh
python dm/harmony/tools/generate_native_art.py --romfs "$ROMFS" --output "$WORK/native-art"
```

This wrapper reuses `dm/ios/tools/native_art/generate.py` and its fingerprint checks, changes only output symbol/resource prefixes, and uses the preserved Harmony glyph-selection implementation. The current upstream native-art manifest differs from the historical Harmony manifest; regenerated binding coverage and image aliases require device revalidation. On Windows, the upstream generator still needs its RomFS/decode environment; the historical private Windows shims are not imported. Do not copy an old generated index into Git to bypass a generator failure.

Export your own `global/excel/experience.txt` from RomFS using your prepared upstream asset tools. It must contain 100 `Level`/`Amazon` rows. Then assemble a new external DevEco project:

```sh
python dm/harmony/tools/prepare_app.py --output "$WORK/app" \
  --runtime "$WORK/execution/build/libxblr_execution.so" \
  --native-art-c "$WORK/native-art/sw_touch_native_art.c" \
  --experience-txt "$WORK/experience.txt"
```

`prepare_app.py` inserts the locally generated experience module and skill index, copies the external runtime library, and writes a local CMake path file. Source checkout files are not modified. Open `$WORK/app` in DevEco Studio, resolve Harmony dependencies, build the default or release product, and configure signing **only in that external project** with your official local account/profile. Both checked-in products are unsigned. The release product's bundle identifier is `com.xiao.boluo`; use your own authorized identity/profile where appropriate.

Game data, converted SPIR-V/shader metadata and the touch overlay are separate user-provided runtime data. The template's import screens are retained. This change does **not** provide a turnkey converter/stager for the historical ASTC/touch overlay or original Maxwell-to-SPIR-V chain. Recreate the overlay with the matching upstream local tools, keep the `sw_native_*` aliases from the generated art directory under the overlay's `touchart/`, and provide it through the existing local import workflow. Those generator/toolchain and overlay version requirements remain an integration gap, not a successful end-to-end reproduction claim. Historical logo/real-2D diagnostic probes are disabled; game graphics remain enabled.

## Verification and remaining contribution work

This submission actually built the synthetic `xblr_runtime`, its integration object, and the complete `xbg_link_check` shared library with the official OHOS Clang **15.0.4**, compatible SDK **26.0.0**, ARM64, Release. The graphics check uses `--no-undefined`; the real execution images, Opus services, NAPI/ArkTS and HAP/App packaging were **not** built. No device was touched. Five pure Node input/import/layout checks passed; the retained screen-awake check fails at its existing assertion on line 12, identically in the untouched historical source. The new experience generator accepted a synthetic complete table and rejected a short table; source-tree overwrite protection was checked. Python source syntax and new CLI help checks passed.

The upstream i18n table check passes (371 rows). On this Windows checkout the untouched base literal gate already reports 460 failures because its source paths use backslashes while whitelist entries use slashes; the post-import run is unchanged. An invocation that normalizes only path separators passes. That gate does not scan this new Harmony directory. The Harmony UI's inherited literals are **not migrated into `dm/i18n/ui_text.tsv`**, and much of the imported code's inherited comments are not bilingual. These are explicit contribution requirements still to finish. No historical UI screenshot is attached as evidence of this reorganized template or its neutral icons. New interface records may also require a maintainer issue before merging.

Historical records, not new validation: the local handoff reports C142 entering a village on Pura X at 26–30 fps when cool, C142 reaching character selection on Mate 90 at 30 fps, and C149 skill binding on Pura X. It also records approximately 21 fps after prolonged heating and incomplete inventory drag/drop, free town portal and experience-HUD work. Current source ancestry and the packaged runtime SHA were checked during import, but historical device measurements were **not rerun** and do not validate these preparation tools. The historical App runtime/build2 SHA256 was `2cfe57b9b25030b63e4c30f13958e596dc6db3fe8200736a1967676b8a615e39`; the binary is excluded.

All copied code remains under the repository license, with the original NVN header notice and `graphics/third_party/` licenses/provenance retained. This source snapshot is reviewable platform work, with the generation, packaging, localization, screenshot and device gaps above still open.
