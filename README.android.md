# D2R Mobile · Android

**Run the Nintendo Switch release of Diablo II: Resurrected (with the Reign of the Warlock expansion) that you own, natively, on an Android phone.** The game's own aarch64 program executes directly on the device with no emulated CPU; the platform services and graphics API are replaced by this repository's host layer, the graphics backend is reimplemented on Vulkan, and the shaders are translated offline.

中文: [README.android.zh-CN.md](README.android.zh-CN.md) · Main repository: [README.md](README.md) · Game files: [docs/GAME_FILES.md](docs/GAME_FILES.md)

> **Status: in development.** The Android build is fully playable (into a game, combat, saves, settings, the complete touch scheme), but it does not yet reach the iOS build's 60 fps — it currently measures about **30 fps** (see "Measured performance" below). The release target and the known limits are both documented here.

---

## 1. What you need

### 1.1 Device

| Item | Requirement |
|---|---|
| OS | Android 10 or later (API 29+) |
| Architecture | arm64-v8a |
| Graphics | Vulkan 1.1 with a vendor driver (**Adreno tested**) |
| Memory | 6 GB or more recommended (the game's RomFS is about 8.4 GB; saves and the shader cache need space too) |
| Storage | 10 GB or more free recommended |

**Tested device**: Xiaomi 22081212C (Snapdragon 8+ Gen 1 / Adreno 730 / Android 16, 12 GB). Every performance figure in this document comes from that phone.

### 1.2 Game files

**You must own the game.** No game files are in this repository and none ever will be. You need the Switch release **1.0.34.0** (corresponds to PC **3.3.94012**), in two parts:

- Program files (ExeFS): `main`, `sdk`, `subsdk0` — about 26 MB
- Update package resources (RomFS): the `data/` directory — about 8.4 GB

See [docs/GAME_FILES.md](docs/GAME_FILES.md) for how to export them from your own console with nxdumptool. **The version must match**, otherwise the generation scripts stop with an error naming the mismatch instead of producing a broken build.

---

## 2. Building

### 2.1 Toolchain

| Dependency | Version |
|---|---|
| Android NDK | 27.2.12479018 |
| CMake | 3.22 or later |
| Ninja | any recent version |
| JDK | 17 (for packaging the APK) |
| Python 3 | for asset generation and the verification scripts |

### 2.2 Steps

```bash
# 1) Generate assets and the ROMFS overlay (needs the game files above)
dm/android/tools/gen_overlay.sh
#    If assets were generated before and you only want to repackage:
#    DM_ASSETS_REUSE=1 dm/android/tools/gen_overlay.sh

# 2) Build the native library
cd dm/android/build && ninja

# 3) Package the APK (output: dm/android/build/apk/dodola.signed.apk)
dm/android/tools/build_apk.sh

# 4) Deploy to the device and launch
ADB=adb ANDROID_SERIAL=<your-device> dm/android/tools/deploy_apk.sh
```

`deploy_apk.sh` pushes the game data (RomFS and the shader cache) into the app's data directory, then installs and starts the APK. Layout:

```
/sdcard/Android/data/org.d2r.binaryprobe/files/d2r-mobile/
├── romfs-view/          game resources
├── shaders/             offline-translated shaders
├── diagnostics/         logs and diagnostic switches (see below)
└── ...
```

The first launch does a slower initial pass (unpacking and indexing resources); after that you are in a game about one second after the main menu.

---

## 3. Controls

- **Touch**: the full scheme — virtual stick bottom-left, skill wheel bottom-right, a bottom bar with
  life/mana/experience/belt/town portal, layout that switches with panel state, skill glyphs and
  charge badges. Bluetooth and USB gamepads work at the same time.
- **Panels and menus**: the same structure as the iOS build; language follows the game setting
  (English / Chinese).
- Touch layout and geometry are produced by the generation scripts and checked by the contract test
  in section 6.

---

## 4. Quality and performance

### 4.1 Resolution tiers (Android only)

The Custom settings page has a **Resolution** row with three choices:

| Tier | Output budget | World (3D) budget | Notes |
|---|---|---|---|
| Power saver | 1.0 MP | 1.0 MP | cheapest, softest picture |
| **Standard (default)** | 1.25 MP | 1.25 MP | about 30 fps on the tested phone |
| High | native output | 0.55 × output | sharpest UI, world slightly below standard |

The output width is derived from the height to keep the display aspect exactly — **no tier distorts the
picture**. Changing the tier requires restarting the game.

Outside the Custom page you can also override with `diagnostics/quality.txt` (one line: `low` /
`standard` / `high`) or with `diagnostics/flag-*` (see `dm/android/app_jni.c` for the full list).
Priority: `DM_GAME_RES` / `DM_3D_SCALE` environment variables > `DM_QUALITY_TIER` > Custom page > default.

### 4.2 Measured performance (22081212C, standard tier, camp scene)

| Metric | Value |
|---|---|
| Frame rate | **29.9–30.1 fps** (SurfaceFlinger actual presentation timestamps) |
| Frame interval | P50 33.06 ms, P95 41.3 ms, dropped frames 0 |
| CPU (game thread) | about 33 ms/frame — replay 16–19 ms, command recording about 3 ms |
| GPU | about 28 ms/frame (80–90% utilisation) |
| Core usage | one render thread is saturated at 100% (the bottleneck); about 2 cores for the whole process |

**World scale versus frame rate** (same camp, stepped through restarts): with the standard output
fixed at 2270×1056, a world scale of 0.722 → 0.90 holds the same frame rate (**81% more world pixels
for free**); only 1.0 costs about 3%. For a sharper picture set `DM_3D_SCALE=0.88` (restart to apply).

### 4.3 Where the gap to iOS is

iOS holds 60 fps; Android is at 30. The gap is not on the GPU side (the GPU still has headroom) but in
**the serial work of one submission thread**: about 1,977 draws and 36,000 commands per frame, with
binding resolution as the main cost. Every optimisation tried and every one ruled out is recorded in
[docs/android-resolution-plan.zh-CN.md](docs/android-resolution-plan.zh-CN.md) (Chinese).

---

## 5. Diagnostics and tuning switches

Several files under `diagnostics/` can be used at runtime (**`chmod 666` them**, or the app cannot read
them):

| File | Purpose |
|---|---|
| `env.txt` | environment variables imported at start (one `KEY=VALUE` per line) |
| `quality.txt` | quality tier: `low` / `standard` / `high` |
| `flag-*` | preset switches (chunk size, scale, present rate, …) |
| `loadstore.txt`, `planstate.txt`, `prereset.txt`, `nooccl.txt`, `chunkd.txt`, … | **in-session A/B** switches: content 0/1 (or a step value), read once per statistics window, no restart needed |

Frequently used environment variables:

| Variable | Effect |
|---|---|
| `DM_GAME_RES=1920x864` | set the output resolution directly |
| `DM_3D_SCALE=0.88` | set the world scale directly (0.70–1.0) |
| `DM_PROFILE_DETAIL=1` | enable the per-stage timers (`RENDER_*_PROFILE` lines) |
| `DM_VK_DEBUG=1` | enable the driver-side message callback (`VK_EXT_debug_utils`, no validation layer needed) |
| `DM_SUB_RING=0` | disable double-buffered submission contexts (on by default; for A/B) |
| `DM_UI_NO_QUERY=0` / `DM_SHADOW_NO_QUERY=0` | restore occlusion queries on the UI / shadow passes (skipped by default) |
| `DM_AFFINITY_ALL=1` | widen the render thread's affinity to every core (off by default) |

**Measure the frame rate with an external ruler**, not only the app's internal counter:

```bash
python3 tools/pf/sf_fps.py <serial> 20
# prints SurfaceFlinger presentation fps, interval P50/P95, dropped-frame rate, and the scene probe line
```

---

## 6. Verification scripts

```bash
python3 dm/i18n/scan_literals.py                  # user-facing literal check
python3 dm/ios/tests/test_touch_layout_options.py # touch layout contract (19 Android rows / 18 iOS rows)
bash dm/android/tools/sync_surface_audit.sh       # surface-sync audit (conflict surface stays 43)
```

---

## 7. Known limitations

- **Performance**: about 30 fps; the 40 fps target has not been reached yet (reasons and ruled-out
  approaches are in section 4 and the linked plan document).
- **Validation layer**: not installed on the device; the built-in driver-side message callback stands
  in for it.
- **Address sanitizer**: `cmake -DDM_ASAN=ON` plus `DM_PACK_ASAN=1 dm/android/tools/build_apk.sh`
  builds an ASan variant (runtime packed into the APK) for memory-error hunting.
- **No game content is distributed**: neither the APK nor the repository contains game data.

---

## 8. License

MIT. The game, its expansion, its assets and its trademarks belong to their respective owners. This
project only provides a compatibility layer that runs the game you own on your own device; it does not
provide or distribute any game files.
