# Building an installable package without Xcode (experimental)

The normal route is to build the whole app with Xcode on a Mac (see "Building" on the front page). This page describes another route: we build, ahead of time, a **"shell" package that contains no game content at all**; you put the content into it on your own computer from your own game files, and then install it on your phone with a common sideloading tool. You never build or sign with Xcode, and you do not need a paid Apple developer account.

中文：[NO_XCODE.zh-CN.md](NO_XCODE.zh-CN.md)

**This is a first version. Be clear about where it stands:**

| Step | Status |
|---|---|
| Shell + tool produce a playable package | Verified with one run each on a Mac and on a real device (character creation, entering the game, audio) |
| No need for Apple's shader compiler | When the computer does not have it (Windows / Linux / a Mac without Xcode), shaders go into the package as source and the phone compiles all of them once on first launch (with a progress bar, see below). This path has been forced on a Mac and verified by entering the game on a real device |
| Running the tool on Windows | Works end to end (Windows 11, Python 3.12): a package from scratch takes about 7 minutes. Compared file by file with a package made on a Mac from the same game files: 14017 files including the game program are byte-identical; the only differences are the digit spacing of HUD numbers and 31 English label images (less than one pixel -- the Windows imaging library lacks a text layout component; play is unaffected) and the overlay archives built from them. A package made on Windows, signed on a Mac and installed on an iPhone: fresh install, importing resources, compiling all 7212 shaders on first launch (about 7 minutes), creating a character, entering a game and changing acts all work. The compiler, .NET and Python were already on that machine; installing the dependencies from nothing was not verified separately |
| Signing and installing with a sideloading tool | **Not verified yet -- please open an issue and tell us how it went.** What we verified: signing with a free Apple ID through Xcode automatic signing, and re-signing with a development certificate to simulate this step; the sideloading steps below follow the usual way these tools are used |

## What the shell contains and does not contain

Download the shell from this repository's [Releases page](https://github.com/jhaizhou-ops/d2r-mobile/releases). There are two on the Releases page: `D2RMobile-shell-build-input.zip` for iOS 26 and later, `D2RMobile-shell-ios18-build-input.zip` for iOS 18 – 25 ([OLDER_SYSTEMS.md](OLDER_SYSTEMS.md)); pick the one for your phone's system, the steps after that are the same. The shell (about 4 MB) contains only our own host program, the icon and two tables that are public in this repository. It does **not** contain: the game program, shaders, the overlay, UI art, or any data table generated from game files. The script that builds the shell (`dm/ios/tools/shell_build.sh`) finishes by searching every file in the shell for real game content, and refuses to produce a shell if it finds any.

**The shell is build input, not an app to install** (which is why it is a `.zip`, not an `.ipa`). If you rename it and install it as is, the app shows a "This Package Is Not Ready" dialog on launch, lists what is missing and points you back to step 1 below; it does not start the game.

**Every time you move to a new release, redo step 1 with the new shell and the source of the same release**, then install the package you just made. The resource archives you put on the phone in step 3 do not depend on the version and do not need to be copied again.

## What you need

- The files of the game you bought: `ExeFS` and `RomFS` of the Switch release 1.0.34.0 (corresponding to PC version 3.3.94012), see [GAME_FILES.md](GAME_FILES.md)
- The source of this repository, at the same release as the shell (the tool checks and says so if they differ)
- Python 3.9 or later, plus: `python3 -m pip install pillow numpy lz4 capstone unicorn opencc-python-reimplemented`
- A C compiler (`clang`; on Windows, installing LLVM is enough) and the .NET SDK. Apple's Metal shader compiler is **not** required: if it is there (a Mac with Xcode) the shaders are compiled into libraries on the computer, otherwise they are compiled on the phone
- `spirv-cross`, **version 1.4.357.0** (release tag `vulkan-sdk-1.4.357.0`). A different version may translate shaders differently -- we measured 231 shaders differing between the latest source and this one. The shell carries a digest of all shader sources; the tool compares after translating and stops with an explanation on a mismatch, rather than producing a package that may render wrong
  - macOS: `brew install spirv-cross`, then check with `brew list --versions spirv-cross`
  - Windows: there is no ready-made installer; build it from source (needs `git`, `cmake`, `ninja` and the LLVM above) and add the directory of the resulting `spirv-cross.exe` to `PATH`:

    ```
    git clone --branch vulkan-sdk-1.4.357.0 --depth 1 https://github.com/KhronosGroup/SPIRV-Cross
    cmake -S SPIRV-Cross -B SPIRV-Cross/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DSPIRV_CROSS_ENABLE_TESTS=OFF
    cmake --build SPIRV-Cross/build --target spirv-cross
    ```
- About 12 GB of disk space (caches and intermediate output, under `DM_ROOT`, `~/dm-work` by default)

The tool lists whatever is missing, item by item, when it starts.

## Step 1: put the game content into the shell

```
python3 dm/tools/d2rm_pack.py <D2RMobile-shell-build-input.zip> <your game files directory> <output.ipa>
```

`<your game files directory>` is the level that contains `ExeFS/` and `RomFS/`. In order, the tool: rewrites the game program and writes it out as a dynamic library → generates UI art and a few data tables from the game files → generates the overlay → translates the game's shaders to Metal → checks against the list inside the shell that nothing is missing → writes the new package (about 130 MB). The first run takes a bit over ten minutes, mostly translating shaders; every step is skipped once done.

Your game files are only read on your computer. **The resulting package contains game content: it is for your own use only. Do not distribute it.**

While translating shaders the tool re-registers the toolchain versions found on your computer, which rewrites two files in the repository (`dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h`). This is expected; ignore it and do not commit them.

## Step 2: sign and install

Open the `output.ipa` from the previous step in a sideloading tool (such as Sideloadly or AltStore), sign in with your own Apple ID and install it on your phone. A free Apple ID is enough; Apple's limits for it are: the signature expires after 7 days (re-sign and reinstall when it does; your saves stay), at most 3 devices, at most 3 apps per device. After the first install, trust it once under Settings > General > VPN & Device Management; if you uninstall it and install again (rather than installing over it), the system revokes that trust and you have to tap it again (measured).

With a free account there is no iCloud save sync; everything else works as usual.

## Step 3: put the game resources on the phone

The package does not contain the 5 GB of resource archives (too large, and they do not belong in a package). After installing, put the 18 `.data` archives from your game files' `RomFS/data` into one folder and copy it into this app's folder with Finder or the Files app; on launch the screen lists how many are present and which are missing.

## First launch: preparing shaders (only for packages made without Xcode)

**This step only exists in packages made on a computer without Apple's shader compiler** -- that is, Windows, Linux, or a Mac without Xcode. In a package built with Xcode (the "Building" route on the front page), and in one made with the packaging tool on a Mac that has Xcode, the shaders are compiled on the computer and shipped in the package: there is no such page, and the first launch goes straight to the main menu.

With a package made on a computer without that compiler, the app stops on a "First launch: preparing shaders" page the first time you open it, with a progress bar and the estimated time left, and goes on to the main menu by itself when done. Measured at about 7 minutes on an iPhone 17 Pro Max (7212 shaders, 410 seconds). Four cores run flat out for those minutes and the phone is fairly warm when it finishes (it reached a moderate thermal level in our run, was not throttled to the hottest level and was not terminated); this is normal. For the first minute or two afterwards the picture may not be fully smooth until it cools down. This step compiles all of the game's shaders on the phone once; the system caches the result, so the game does not stutter because of shaders afterwards.

- It is only needed once. It runs again after a system update, after reinstalling the app, or after the system has cleared its caches.
- You can switch away or close the app; when you come back it counts from the start again, but each shader already compiled takes less than a millisecond, so in effect it continues.
- The phone gets warm during these minutes; keeping it plugged in and out of a thick case helps.

## Older devices (A16 and earlier chips)

The iPhone 14 family, iPhone 15 / 15 Plus and earlier models need an extra texture pack, passed in at step 1. The full explanation is in [LEGACY_DEVICES.md](LEGACY_DEVICES.md); on this route it goes like this:

1. Get the astcenc encoder: download the archive for your system from the upstream release page and unpack it (<https://github.com/ARM-software/astc-encoder/releases>; on Windows use `bin/astcenc-avx2.exe` from `astcenc-…-windows-x64.zip`).
2. Make the texture pack (three commands; `<game files directory>` as above, `D` is an output directory of your choice):

   ```
   python3 dm/tools/astc_pack.py <game files directory>/RomFS D/work --astcenc <path to astcenc> --preset thorough
   python3 dm/tools/casc_overlay.py pack <game files directory>/RomFS D/pack D/work/repl.tsv 0-8
   python3 dm/tools/astc_pack.py finalize D/pack
   ```

3. Pass it when packaging: set the environment variable `D2RM_ASTC_PACK` to the full path of `D/pack`, then run `d2rm_pack.py` as in step 1. The tool prints a line saying older-device support is included.
4. When you put the game resources on the phone in step 3, also copy the whole `D/pack/Legacy Device Pack` folder to the phone, at the same level as the game resource folder.

The texture pack and the install package are a matched pair: if you remake the pack, remake the package.

## When something goes wrong

- The tool says the shell and this source are not the same version: use the source of the same release as the shell.
- Launch says the package has no game program yet: you installed the shell itself, not the output of step 1.
- The log after launch contains "没读全" (not fully read) or "没有 dm/derived.tsv" (no dm/derived.tsv): step 1 did not complete; run it again.
- When trying the package directly on a Mac: do not keep it under a temporary directory such as `/tmp`; the system leaves it stopped before launch. Put it somewhere in your home directory.

## Building the shell yourself

`dm/ios/tools/shell_build.sh` builds the shell. The machine that builds it needs the game files and the full build environment (the same as a normal build), because the real generated output is used to size a few tables, to determine the list of what the shell lacks, and to run the final content check.
