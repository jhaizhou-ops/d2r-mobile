# Android port

中文：[README.zh-CN.md](README.zh-CN.md)

This is the ARM64 Android version. The APK links the shared host, the Vulkan
replay engine, AAudio, a transparent touch overlay drawn the same way as iOS, and
system text input. It has been played on real phones (camp, fights, panels, NPC
dialogs); measurements are in `docs/android-notes/`. Players
install the release APK and import their own game content: see "Installing as a
player" below.

## Source layout

- `app/`: Java Activity, fullscreen SurfaceView, touch overlay, and Gradle app.
- `jni_android.c`, `front_android.c`: startup, resource locations, lifecycle, and
  platform front-end interfaces.
- `touch_android*`: bridge to the shared `dm/ios/Sources/dm_touch_controls.c` state
  machine; `text_input_jni.c` bridges synchronous guest prompts to Android dialogs.
- `vulkan_present.*`: Android window ownership, swapchain, and presentation.
- `../gfx/vulkan/`: NVN replay adapter and Vulkan engine.
- `compat/`: Android implementations of the host's Apple platform interfaces.
- `tools/`: source-list parsing, ELF assembly conversion, and localized resources.

## Installing as a player

The release page's `D2RMobile-android.apk` contains no game content (built by
`dm/android/tools/shell_apk.sh`: the three tables generated from game files are
replaced by stand-ins that read the imported `derived.tsv` at run time, as in the
iOS shell). You make the game content on a computer from your own game files:

```sh
python3 dm/tools/d2rm_android.py <Update directory of the Switch version 3.3.94012> <output directory>
```

It rewrites the game program for Android, translates every shader into a Vulkan
bundle, generates the touch overlay and a few data tables, and writes
`<output directory>/android/`. The tools it needs are listed at the top of the
script (Python packages, the .NET SDK, and `spirv-opt` / `spirv-val` / `glslc`
from the Vulkan SDK); it reports anything missing.

Copy the whole `android` folder to `Download/S2X/D2R Mobile/` on the phone, and
your game resources (the whole `RomFS/data` directory) to `data` in the same
place. Open the App: when it finds a new import pack it copies it into the
internal `files/dm/` (with progress; the first time takes a minute or two), then
starts. The import pack must come from the same release as the App (the product
code in `pack_info.txt` must equal the one built into the App); otherwise the App
says so. After the import you can delete the `android` folder on the phone. The
import pack contains game content; keep it to yourself.

## Build locally

The current configuration uses JDK 17 or newer, Gradle 9.8.1, Android SDK 36,
NDK 29.0.14206865, CMake 3.31.6, and Python 3. ARM64 devices must support Android
12/API 31 or newer and Vulkan 1.1 with the required device features. These are build/manifest requirements, not a
claim that all matching devices have been tested. The first target is vivo X200
Ultra (Android 16/API 36, Adreno 830).

Vulkan compatibility is selected from actual format features, not the Android version. Startup logs `format_feature_flags2` and `depth16_as_d24`; D16 is widened only when D16 lacks linear filtering and X8_D24 has all required uses. A Vulkan 1.1 version number alone does not establish compatibility. vivo should log `1 / 0`; the Redmi K50 Ultra (Adreno 730, Vulkan 1.1) logs `0 / 1` and runs this path.

Prepare your own game files according to the root setup documentation. Generate
`dm_vault_tab.h`, `dm_hud_exp.h`, and `dm_bright_levels.inc` outside the repository
using the rules in `dm/host/Makefile`. Set `ANDROID_HOME` and `ANDROID_SDK_ROOT` to
the same SDK directory, and `JAVA_HOME` to the JDK. From the repository root:

```sh
gradle -p dm/android :app:assembleDebug :app:lintDebug \
  -PdmGenDir=/absolute/path/to/private/generated/headers \
  -PdmPython=/absolute/path/to/python
```

The APK is `dm/android/app/build/outputs/apk/debug/app-debug.apk`. Native linking
uses `--no-undefined`. `-PdmPrebuiltJni=...` is a packaging-only fallback; it is not
evidence that current native sources compile.

On Linux or WSL, `dm/android/docker/` runs the same build in a container with these
versions pinned, so no Windows-side Python, NDK or CMake is needed:

```sh
docker build -t dm-android-build:1 dm/android/docker
DM_GEN_DIR=/absolute/path/to/private/generated/headers dm/android/docker/build.sh
```

The script runs as your user and keeps the debug signing key in `~/.android-dm`; see
its header for the mounts.

## Private runtime files

The APK and Git repository contain no game program, shaders, game art, or saves.
The import pack provides them (see "Installing as a player"), and the App imports
it into the internal directories below.

| Location | Contents |
| --- | --- |
| App internal `files/dm/out/` | `main` and `sdk` rewriter products generated with `DM_TARGET=android`, including patched modules, fixups, and gates |
| App internal `files/dm/shaders/` | External `.xgs` bundles matching the game and Vulkan backend |
| App internal `files/dm/overlay/` | Required CASC touch overlay, including `overlay.manifest`, generated from the matching source and the user's resources |
| App internal `files/dm/overlay/touchart/`, `files/dm/overlay/hud_v4_ui/` | Skill atlases and their silver silhouettes, map icon, circle frame, bottom bar art, and the skill-button socket (the repository's `dm/ios/Sources/ControlArt/dm_mobile_socket.png`), staged by `stage_touch_assets.py`. The touch controls draw them the same way as iOS (`dm/ios/Sources/DmTouchControlArt.h`, `DmTouchOverlay.m`); the joystick is drawn in code |
| App internal `files/dm/overlay_variants/` | Optional: a complete overlay per combination of the Custom switches that go through the overlay (now only "Item notes and old names"), each with its own `overlay.manifest`; at start the App layers the one matching the switches in front of `overlay/` |
| Shared `Download/S2X/D2R Mobile/data/` | User's resource archives and `.build.config`; requires Android's all-files-access setting. When this directory does not exist but the earlier `Download/data/` does, the App keeps using `Download/data/` |
| App internal `files/dm/derived.tsv`, `files/dm/pack_info.txt` | Data tables generated from game files (read at startup by the release APK without game content); the import pack's product code and creation time |
| App internal `files/save/` | Saves; preserve during upgrades |
| App internal `files/temp/`, `files/dm/boot.log` | Temporary files and appended startup logs |

The launcher uses external app-private `dm/` as a fallback if no internal main
module exists. It checks required paths before startup; this check does not prove
shader identity or resource compatibility. Use the debug application's `run-as`
access to provision internal files, and overlay-install with the same signing
key. Never uninstall or clear data as an upgrade step. Back up existing saves
before device validation. Do not upload APKs or any of these private files to a PR.

The overlay generator `dm/ios/tools/gen_romfs_overlay.py` supports
`DM_DESWIZZLE_LIB` for a locally compiled native texture helper. Its thin CASC
output contains only changes and references the original archives; it must not
replace the original resource directory.

### Shader bundles

`files/dm/shaders/` holds one `<key>.xgs` for each shader the game uses; the
key is the first 16 hexadecimal digits of the SHA-256 of the shader's control
section. A bundle is an XGS1 file: a header, the bindings, the vertex input
locations, the c1 bytes, then the SPIR-V (`xbg_shader_header` in
`dm/gfx/vulkan/engine/xbg_game.h`). The set tested so far has 6,931 bundles:

- **6,927 are translations of the game's own shaders.** They are derived from
  game files, so they stay out of Git, PRs and the APK. `dm/gfx/tools/dmsh_vk.py`
  makes them from start to finish: `dm/gfx/tools/dmsh_xlate` translates each
  shader to SPIR-V for Vulkan, the bindings are renumbered for the single
  descriptor set (original number × 6 + stage number), the capabilities for
  reading and writing storage images of unknown format are added, unused
  capabilities are trimmed, `<key>.vk.json` is exported, and the packing function
  of `pack_game_shaders.py` writes the bundle. The 6,927 bundles it makes are
  byte-for-byte identical to the tested ones made with biglawine's HarmonyOS
  shader pipeline (`dmsh_vk.py --compare <reference dir> <output dir>`).
- **4 are the query kernels embedded in the main program** (`q_val0`,
  `q_val8`, `q_dlt0`, `q_dlt8`; Mac and iOS use the MSL versions in
  `dm/gfx/nvn/nvn_builtin.h`). They are hand-written, so they can be built from
  source: `python3 dm/gfx/tools/pack_builtin_queries.py --output <new
  directory>` compiles `dm/gfx/vulkan/kernels/builtin_queries.comp` with
  `glslc` (the NDK has it under `shader-tools/`) and writes the 4 files.

A bundle set can also carry `vk-pipeline-list.seed`. On a first launch, before
`files/dm/vk-pipeline-list.bin` exists, the engine builds the pipelines the seed
lists during start-up, so a first visit to the places it covers does not
stutter while pipelines build. `dm/android/tools/merge_warm_lists.py` merges
several devices' `files/dm/vk-pipeline-list.bin` into one seed. A seed only
works with the bundles it was recorded with, so it ships with them:
`dm/tools/d2rm_android.py --warm-seed <file>` puts one into the import pack,
checks it before the slow steps, and records it in `pack_info.txt` as
`WARM_SEED=<sha256 prefix>`. Keep seeds outside the repository. The seed only
helps a first launch: a phone that already has `files/dm/vk-pipeline-list.bin`
keeps using its own list after a re-import. The tool's check runs with
`make -C dm/host/tests run` (or `python3 dm/tools/tests/test_warm_seed.py`).
The measurements are in `docs/android-notes/2026-10-22-first-play-stutter.md`.

## Screens narrower than 16:9 (foldables)

A foldable inner panel (vivo X Fold3: 2480x2200, aspect 1.127) is narrower than 16:9, so a 16:9 picture leaves black bars at the top and bottom. `front_android.c` therefore turns on the same "true fullscreen" the iOS (`front_ios.m`) and HarmonyOS (`front_ohos.c`) ports use: in a run the picture is output at the screen aspect and the 3D fills the screen, the host lays the game UI out at 16:9 anchored to the bottom (`dm_uiscreen.c`), and the Vulkan engine shifts that UI draw pass down (`xbg_set_ui_rect_fn`). Front-end pages and the loading screen stay 16:9 centered with margins, because they are designed as whole 16:9 pages.

The touch overlay (`TouchOverlay.java`) and the native touch layout follow `front_android_view_rect`: the bottom 16:9 UI rectangle in a run, the centered 16:9 block outside it, the whole window on screens wider than 16:9 (where nothing changes). Drawing, hit tests and `dm_ui_*` coordinates live inside that rectangle; touches above it are still world taps (the game side receives a negative y), and world taps convert back to picture coordinates (`world_rect`/`world_norm`, as in `touch_ohos.c`).

The output size is the screen aspect with a 1080-line short side by default, and the 3D scale defaults to about 700 lines (0.65): the same GPU budget as the old 720p x 0.75, with a UI 2.25x sharper than 720p. Native pixels would lift the 3D from 540p to 1100p, because the game's 3D scale floors at 0.5 (`hle_oe.c`), which no Snapdragon 8-class device holds at 60 fps. `DM_ANDROID_RES=<short side>` overrides the profile, `DM_NATIVE_RES=1` uses the screen's native pixels, and `DM_ANDROID_FULL=0` turns true fullscreen off (16:9 with bars everywhere, like `DM_IPAD_FULL=0` on iPads).

The game floors its UI height at 720 pixels (`dm_uiscreen.c`). Measured on 3.3.94012 by writing 684 / 756 / 792 and reading back the HUD: the orb-centre gap scales as 0.667x the height the game actually lays out, so 684 is laid out as 720 — the UI then overflows the bottom of the picture and every overlay copy sits a few percent off. The host therefore writes `max(width*9/16, 720)` (and caps the front-end render height the same way, so the character-select page no longer overflows); `DM_UI_MIN_H` overrides the floor. The vertical offset, the overlay's view rectangle and the UI-text / tap conversions all follow that floored value.

Two engine details come with it (`xbg_game.cpp`): UI draws binding the same projection constant buffer as a recognised UI draw are shifted as well (the minimap's geometry uses a different vertex signature), and a top widget the host lifted above the UI rectangle receives an empty scissor from the game — the engine replaces it with the picture's top strip instead of dropping it, which is what made the minimap disappear in true fullscreen.

The overlay has to follow the game on every edge, and the horizontal reference turned out not to be a constant: the game lays its HUD out for 16:9, so the HUD centreline always sits 1920 layout units (= 1173.6 mockup pixels) from the UI rectangle's left edge. On a 16:9 rectangle that is the rectangle's centreline, and on a narrower one -- which is exactly what the 720-pixel floor produces -- it sits right of it. `front_android_view_rect` therefore derives that difference from the current resolution and shifts the whole overlay's view rectangle by it (the self-drawn HUD, the buttons and the touch mapping all move together, while `world_rect` adds `-vr_x` back so world taps are untouched). Verified on the simulated Galaxy Z Fold 8 inner panel (2448x1848, aspect 1.325) and cover display (1972x1248, 1.58), where the offset computes to 0, and on 2480x2200 where it is the ~50 pixels the HUD contents needed.

Two more measurements from the same 2480x2200 panel: the game's own HUD in true fullscreen is centred 44 mockup pixels right of the 16:9 mockup's centreline (1434), so the self-drawn cells, numbers and badges sat about 50 screen pixels left of the game's belt content (`TouchOverlay.hudX` now draws on the game's reference in that case); and the top-right minimap plus the area-name label were drawn past the picture's right edge, so a true-fullscreen run reports a small default right safe area (`touch_android.c`) that moves that block inward. Wide screens keep both values at 0 and are unchanged.

Verified on the Xiaomi 12T Pro at 2480x2200 (HUD cells and belt content aligned, minimap visible, area name fully on screen), at 2480x1995 / 2480x1900, and at native 2712x1220.

The game only accepts a render size at or below the window texture it created at startup (`dm_uiscreen.c`), so the first value reported is the cap and later values are the current size. Unfolding / folding recomputes the current size, scales it into the cap and pushes notification 31 so the game rereads. Started on the wide outer screen, unfolding can only scale into the cap reported at startup: the picture fills but is softer than starting unfolded. The regression cases are in `dm/android/tests/test_touch_snapshot.c` (`android_view_rect_coordinates`, `android_world_tap_fullscreen`).

## Validation and remaining work

Run the root contribution checks, plus `:app:lintDebug`. The pure Java input
checks can run without a device:

```sh
javac -encoding UTF-8 -d /tmp/dm-input-tests \
  dm/android/app/src/main/java/com/xiaoboluo/d2r/TextInputRules.java \
  dm/android/tests/TextInputRulesTest.java
java -cp /tmp/dm-input-tests com.xiaoboluo.d2r.TextInputRulesTest
```

Save export / import (`SaveTransfer.java`, see "Backing up and moving saves" in
`docs/ANDROID.md`) is tested in a temporary directory the same way:

```sh
javac -encoding UTF-8 -d /tmp/dm-save-tests \
  dm/android/app/src/main/java/com/xiaoboluo/d2r/SaveTransfer.java \
  dm/android/tests/SaveTransferTest.java
java -cp /tmp/dm-save-tests com.xiaoboluo.d2r.SaveTransferTest
```

Device acceptance must separately record installation, genuine GPU output/FPS,
character creation, entering a world, moving and stopping, NPC interaction,
inventory/skills, sound, background/resume, and saving/reloading. Headless host
FPS is not a rendering result. Metal GPU timing and scanout timestamps are not
implemented by this backend; unavailable metrics must not be treated as zero cost.

The Vulkan bridge currently replays all draws and uses synchronous host
translation (`DM_NVN_SYNC=1`); the upstream asynchronous path needs an independent
object-lifetime audit. The presenter still waits for its queue each frame.
Advanced iOS skill artwork, empty-slot skill menus, specialized quantity/stat
popovers, and shop drag-follow visuals are incomplete. Generic controls and text
input do not establish complete iOS parity. macOS/iOS regressions must be run on
an Apple build host before upstream integration.

The overlay generator writes a source fingerprint and exact file hashes to `overlay.manifest`. The APK embeds the same generator-input fingerprint. Missing, incomplete, or mismatched overlays stop startup with a localized message before the host thread starts. Deploy the complete generated directory; do not copy a manifest onto old resources or generate one on the phone. Regenerate after generator/layout changes. This contract does not authenticate third-party resource downloads.

`gen_romfs_overlay.py` requests overlay-only assets: it skips the iOS skill atlas and its unused skill silhouettes. Full `gen_assets.ensure()` still requires them. Skill atlas validation compares dimensions and decoded RGBA pixels while retaining source sprite SHA256/MD5 checks. Generator subprocesses explicitly use UTF-8/LF and preserve an existing read-only resource adapter. Windows/Linux byte equality must still be verified with the same game input and tool versions.

Vulkan compatibility is selected from actual format features, not the Android version. Startup logs `format_feature_flags2` and `depth16_as_d24`; D16 is widened only when D16 lacks linear filtering and X8_D24 has all required uses. A Vulkan 1.1 version number alone does not establish compatibility. vivo should retain `1 / 0`; Redmi K50 Ultra needs separate device validation.
# Current iOS touch controls

Android shares the upstream touch state machine and its read-only skill-binding observer. Generate the binding index and atlas PNGs from your own matching RomFS outside the checkout:

```
python dm/ios/tools/native_art/generate.py <RomFS> --output-dir <external-native-art>
```

Copy only `dm_touch_native_art.c` into the existing `DM_GEN_DIR`. CMake requires that external index alongside the existing generated host headers. After generating a fresh overlay and HUD, run:

```
python dm/android/tools/stage_touch_assets.py --native-art <external-native-art>/TouchArt --hud <assets>/NativeHUD/v4/hud_v4_ui --overlay <candidate-overlay>
```

Import the complete finalized candidate overlay. Skill PNGs and HUD images stay in the external resource payload; do not add them to Git or APK assets. The renderer reads them from the validated imported `files/dm/overlay` directory. `DM_NATIVE_ART_DIR=<external-native-art>` lets the native-art capture test verify the external index and pixels. The five navigation buttons remain enabled until the complete replacement inventory flow passes device validation.

## Debug performance controls

Only CMake's Debug configuration reads `<files>/dm/android-debug.env`; Release contains no reader or call. This file is separate from the game's general `env`. It accepts only `DM_3D_SCALE`, `DM_3D_SCALE_DYN` and `DM_3D_SCALE_FLOOR`, one `KEY=VALUE` per line. Blank lines, `#` comments and CRLF are accepted. Unknown or repeated keys, files over 4096 bytes, lines over 127 characters, non-finite or malformed numbers, invalid switches and a floor above the starting scale reject the whole file. Scale and floor range from 0.50 through 1.00; DYN is exactly 0 or 1. The log reports acceptance/rejection without values.

Defaults are a scale ceiling of 0.85 on Android (0.75 on the other phone platforms), dynamic scaling enabled, floor 0.60. For a fixed-scale A/B comparison, use:

```
DM_3D_SCALE=0.75
DM_3D_SCALE_DYN=0
DM_3D_SCALE_FLOOR=0.60
```

Synchronization stays enabled. The file cannot override synchronization, frame overlap or any other setting. The parser regression is `dm/android/tests/test_debug_env.c`; build and run it independently from the game.
