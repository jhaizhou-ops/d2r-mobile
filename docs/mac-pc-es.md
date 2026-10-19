# Mac desktop PC profile and Spanish support — integration draft

This proposal brings the locally exercised desktop profile into one review branch. It is an oversized integration draft, not a request to merge without review. It targets Switch build 3.3.94012 and extends build-specific hooks already used by this project. Maintainer agreement on the new interface offsets and the final product generation is required.

## Scope

- Original PC HUD, panels, horizontal belt and skill selectors, without mobile controls or the controller selection ring.
- Point-and-click world movement, NPC menus, native PC inventory/equipment, held-item rendering, drag/drop, stash pages, vendor/modal routing, native keybindings and quick cast.
- One pointer coordinate contract across Retina, letterboxing, resizing and UI/world boundaries; queued event coordinates and one-shot held-item drop behavior.
- Mac window/maximize/fullscreen/render-resolution controls, game cursor, precise scroll accumulation and momentum filtering.
- Spanish port strings and annotations, collection labels, grammar-safe item notes, native level names, centered difficulty and Chronicle filters/search layout.
- VP8 cinematic decoding through FFmpeg, audio-focus routing, standalone Mac packaging, local icon/cursor generation and dependency bundling.
- Host, original-instruction and layout regression tests. No game files, artwork, shaders, saves, keys, logs or built app are included.

The commercial text, voice files, layouts and artwork are read from each user's own game files. Cursor PNGs are generated into the external workspace cache at build/test time. Linked packages reference that local cache through paths.txt; they do not copy cursor images. Standalone packages include derived files only for private local use and must not be distributed.

## Prerequisites and opt-in

Use the normal setup for this repository and the exact supported game build. The Mac additions require pkg-config, FFmpeg development libraries (libavformat/libavcodec/libavutil/libswscale) and dylibbundler, as well as the existing Xcode/Metal/Python dependencies. Install them through your chosen package manager before building; setup.sh does not yet install or comprehensively check these additional dependencies. FFmpeg and all linked libraries keep their own licenses; review the license configuration of your chosen FFmpeg build before distributing any host-only package.

The desktop overlay is opt-in. A regular Mac overlay is not automatically the PC profile:

```bash
export DM_ROOT="$HOME/dm-work"
export DM_OVERLAY_PC=1
./setup.sh
make -C dm/host dmhost
DM_MAC_LINK=1 bash dm/tools/mac_app.sh "$DM_ROOT/out/mac-pc-review"
```

Keep the current rewritten program, generated overlay, PRODUCT and shader reflection/toolchain identity together. Generate the three shader-chain files locally as normal; they are deliberately absent from this PR's changes. Do not reuse a stale product cache. A private standalone package uses DM_MAC_LINK=0; optional DM_MAC_ROMFS_COPY=1 and DM_MAC_ICON_SOURCE point to the user's own local resources. Never upload that package.

The native in-game Options/Controls screen owns gameplay bindings. Select the Resurrected preset and apply it for skill keys 1–6 and potion keys QWER; enable Quick Cast and active-skill bindings if desired. The Mac controls sheet only assigns the two physical mouse buttons. Existing saves/settings must be backed up before trying this branch; use a separate DM_APP_SUPPORT for test launches, then validate the real profile separately. Do not publish saves or preferences.

## Reproduction and validation boundaries

Run the CONTRIBUTING.md checks against this branch and report unrun checks separately. Host tests now generate the cursor cache from the local RomFS. The vault tests declare their generated-header dependency. Historical comparisons are optional, configured with DM_TEST_PREVIOUS_NOTES, DM_TEST_LEVEL_REFERENCE, DM_TEST_PAUSE_REFERENCE or DM_TEST_COLLECTION_OVERLAY; historical workstation folders are not required.

```bash
make -C dm/host/tests run
make -C dm/host/tests movie-real MOVIE_WEBM=/path/to/local.webm MOVIE_NV12=/path/to/first-frame.nv12
```

The second target compiles the FFmpeg path explicitly. The ordinary legacy movie unit test alone does not cover it. A changing VP8-only synthetic video of at least 31 frames and an independently decoded first NV12 frame are sufficient for the protocol test; no commercial video needs to be shared.

The originating local build exercised NPC speech/trade, inventory held sprites and drag/drop, stash transfers, standstill/quick cast, enemy-hover clearing, Chronicle tabs/search, difficulty layout, window modes, native keybinding changes and persistence, cold start and normal paused save/exit. Those observations concern the tested local build, not every act or this rebased/export-adjusted branch. Physical trackpad ergonomics and every audio clip/output device are not certified by simulated wheel events or PCM checks. Raw screenshots/logs are intentionally not attached because they may contain private character or path information.

## Duplicate audit and remaining integration work

The review base is main 9393b00ea9aca52d9254214da93738f26582562f. Existing #65 mouse latency, #66 weak imports, #67 transactional legacy commit, #68 texture stride and the audio downmix discussed in #38 are already in the base, not resubmitted here. #50 is a touch/iPad tooltip docking change; this branch adds separate PC routing while retaining the existing touch route. #82 changes touch close-button snap targets and is not copied here. The current iOS MixWithOthers change is preserved from main.

Before merge: maintainers should agree the desktop hook/layout approach and new build-specific interface information; migrate the remaining literal Mac labels into the string table; align all added/changed comments and file headers with the repository's Chinese/English convention; finish setup dependency checks; run iOS/tvOS and fresh rewrite/graphics/full acceptance for the integrated branch; attach privacy-reviewed visual evidence. Do not infer those checks from the originating local acceptance or this draft's source-only checks. Splitting into focused follow-ups remains a maintainer option; only one draft PR is proposed to avoid duplicate submissions.
