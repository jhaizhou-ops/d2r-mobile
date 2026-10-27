# Dependencies

Everything the build needs is on this page: what each item is, what it is for, where to get it and how to install it. The versions listed are the ones we actually build with; newer versions usually work, and if something breaks, go back to the listed version first.

中文：[DEPENDENCIES.zh-CN.md](DEPENDENCIES.zh-CN.md)

## What you bring yourself

| What | Used for | Notes |
|---|---|---|
| An Apple silicon Mac | Building, and running the host tools | Needs macOS and the Xcode version below |
| Your own game files | The game program and all game data | This repository contains no game files. You need the program files (ExeFS) and the update data (RomFS) of the Switch release 1.0.34.0 (corresponding to PC version 3.3.94012), dumped from a copy you bought and own. See [GAME_FILES.md](GAME_FILES.md) |
| An Apple developer account | Signing the app and installing it on your own device | A paid account gives you everything. A free account (personal team) cannot sign iCloud, sustained execution or the increased memory limit: building with `DM_FREE_ACCOUNT=1` removes them; what you lose is iCloud save sync, and the game runs as usual. Apps signed with a free account must be reinstalled every 7 days. With a free account: before building, connect the phone by cable and unlock it, and set `DM_IOS_UDID` in the local configuration (the build targets this phone, which is what makes Apple generate a provisioning profile for a free team); after installing, before the first launch, trust your developer account on the phone under Settings → General → VPN & Device Management. This mode has been signed with an actual free account, installed and played (on a 12 GB device); 6 GB / 8 GB devices have not been verified |

## Build tools

| Tool | Version | Used for | Official site | Install |
|---|---|---|---|---|
| Xcode (with the Metal compiler) | 26.6 (27.1 to fill the iPhone Duo inner screen) | Building the app and compiling shaders into Metal libraries; with 27.1, `./setup.sh` recompiles the shaders with its compiler | <https://developer.apple.com/xcode/> | App Store or the developer site |
| Homebrew | — | Installing the command-line tools below | <https://brew.sh> | The one-line command on its home page |
| XcodeGen | 2.45.4 | Generating the Xcode project from `project.yml` | <https://github.com/yonaskolb/XcodeGen> | `brew install xcodegen` |
| .NET SDK | 10.0 | Building the shader translation tool | <https://dotnet.microsoft.com/download> | `brew install dotnet` |
| SPIRV-Cross | 1.4.357 | Turning translated SPIR-V into Metal Shading Language | <https://github.com/KhronosGroup/SPIRV-Cross> | `brew install spirv-cross` |
| Python 3 | 3.14 | Running the generation and verification scripts | <https://www.python.org/downloads/> | Ships with macOS, or `brew install python` |

## Python packages

| Package | Version | Used for | Link |
|---|---|---|---|
| Pillow | 12.1.1 | Generating interface art from the game files | <https://pypi.org/project/pillow/> |
| numpy | 2.4.4 | Same | <https://pypi.org/project/numpy/> |
| lz4 | 4.4.5 | Unpacking the program files | <https://pypi.org/project/lz4/> |
| capstone | 5.0.9 | Disassembly, for rewriting the program and for verification | <https://pypi.org/project/capstone/> |
| unicorn | 2.1.4 | Checking, site by site, that rewritten code behaves the same | <https://pypi.org/project/unicorn/> |

All at once: `python3 -m pip install pillow numpy lz4 capstone unicorn` (`./setup.sh` creates an isolated environment and installs them for you)

## Additionally for Android

Players do not build the Android version: they install the APK from the release page and make the import pack with `dm/tools/d2rm_android.py` (which needs Python packages, the .NET SDK and `spirv-opt` / `spirv-val` / `glslc` from the Vulkan SDK; see the top of the script). Building the APK yourself needs the tools below (the port's authors build and test it on Linux / WSL2), with the versions listed in [dm/android/README.md](../dm/android/README.md); on Linux / WSL you can instead use the build environment in `dm/android/docker/`, which pins every version, and skip installing the tools below.

| Tool | Version | What it is for | Official site |
|---|---|---|---|
| JDK | 17 or later | Running Gradle | <https://adoptium.net> |
| Gradle | 9.8.1 | Building the APK | <https://gradle.org> |
| Android SDK | 36 | Compiling and packaging | <https://developer.android.com/studio> |
| Android NDK | 29.0.14206865 | Compiling the host and the Vulkan graphics layer | <https://developer.android.com/ndk> |
| CMake | 3.31.6 | Build scripts for the native code | <https://cmake.org> |
| adb | Ships with the Android SDK | Logs and pushing files while developing (players do not need it) | <https://developer.android.com/tools/adb> |
| Docker (optional) | — | Using the ready-made build environment in `dm/android/docker/` | <https://www.docker.com> |

## Optional

| Tool | Used for | Official site |
|---|---|---|
| astcenc | Converting textures for older devices without BC texture support (A16 and earlier); built from source — how to build it and the whole procedure are in [LEGACY_DEVICES.md](LEGACY_DEVICES.md). We use v5.2.0 | <https://github.com/ARM-software/astc-encoder> |
| hactool | Needed by `dm/tools/extract_game.sh` to extract files from your game package; not needed if your game files are already prepared some other way. We tested with upstream commit `1d64a83` | <https://github.com/SciresM/hactool> |
| FFmpeg | Optional VP8 cutscene playback, off by default; default builds and our release packages do not contain it. Mac (`MOVIE_FFMPEG=1`): `brew install ffmpeg pkg-config`. iOS (`DM_MOVIE_FFMPEG=1`): `dm/ios/tools/build_ffmpeg_ios.sh` downloads the official FFmpeg 7.1.1 source tarball from ffmpeg.org, checks its SHA-256 and builds static libraries with only the Matroska demuxer, VP8 decoder and swscale; see [dm/host/MOVIE.md](../dm/host/MOVIE.md). License: that configuration does not enable GPL or non-free parts, so the libraries are LGPL 2.1 or later; the Homebrew build may enable GPL parts (check `ffmpeg -L`). If you distribute an app that statically links FFmpeg, you must meet the LGPL terms yourself (provide the FFmpeg source and let recipients relink the app with a modified FFmpeg) | <https://ffmpeg.org> |

## Extra tools for on-device and verification scripts

Not needed to build the app or run the Mac smoke test; only for on-device smoke tests, UI regression and the scripts under `dm/verify/`.

| Tool | Used for | Install |
|---|---|---|
| timeout | Wall-clock limits for runs and builds (used by several scripts) | `brew install coreutils` |
| iproxy, ideviceinfo | Forwarding the log port over USB and reading battery level in on-device smoke tests | `brew install libimobiledevice` |
| llvm-objdump, llvm-objcopy | Disassembly and section extraction in the rewrite-equivalence checks | `brew install llvm` |

## Third-party code in this repository

These are already in the repository; nothing to download. Each keeps its license in its own directory.

| Path | What | License | Upstream |
|---|---|---|---|
| `dm/third_party/bcdec` | BC texture decompression | MIT or Unlicense (your choice) | <https://github.com/iOrange/bcdec> |
| `dm/third_party/flip` | Image difference metric | BSD 3-Clause | <https://github.com/NVlabs/flip> |
| `dm/third_party/libdeflate` | Decompression | MIT | <https://github.com/ebiggers/libdeflate> |
| `dm/gfx/vulkan/engine/third_party/bcdec` | BC texture decompression in the Android Vulkan graphics layer (same project as the bcdec above, pinned separately) | MIT or Unlicense (your choice) | <https://github.com/iOrange/bcdec> |
| `dm/gfx/vulkan/engine/third_party/pillow_reference` | Pillow's BC6 decoding reference tables, used only by the Vulkan graphics layer's tests | HPND (Pillow's license) | <https://github.com/python-pillow/Pillow> |
| `dm/third_party/spirv-tools` | SPIRV-Tools optimizer library and the SPIR-V headers it includes, built into the Android package to inline shader helpers on the Adreno 750 family | Apache 2.0 (SPIR-V headers: Khronos MIT-style) | <https://github.com/KhronosGroup/SPIRV-Tools>, <https://github.com/KhronosGroup/SPIRV-Headers> |
| `dm/third_party/ryujinx` | The shader translator from Ryujinx 1.3.2 (three projects, unmodified), which translates the original Switch shaders to SPIR-V | MIT | <https://github.com/Ryubing> |
| `dm/gfx/nvn/include/nvn.h` | Graphics interface declarations | MIT | Author named in the file header |
| Layout size algorithm in `dm/gfx/nvn/nvn_tex.c` | Ported from deko3d | zlib | <https://github.com/devkitPro/deko3d> |
| The 7 rewrites in `dm/tools/patch_nso.py` that let the game run offline | Taken from a community offline fix (LFS Mod Offline Fix) | Unknown | We have no reliable record of where it was first published or by whom; if you know, please open an issue, see [DERIVED.md](DERIVED.md) |
