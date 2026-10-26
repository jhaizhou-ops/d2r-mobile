# dm/ — rewriter, host, graphics layer, iOS shell and verification

中文：[README.zh-CN.md](README.zh-CN.md)

This is all of the code of D2R Mobile. This page explains how the whole thing works, what each directory is, the common commands, and what each of the main files in `host/` is responsible for.

The steps for preparing the environment and building the app are not repeated here:

- Overview and the three build steps: [../README.md](../README.md)
- Dependencies and versions: [../docs/DEPENDENCIES.md](../docs/DEPENDENCIES.md)
- How to prepare the game files and where to put them: [../docs/GAME_FILES.md](../docs/GAME_FILES.md)
- One command to prepare the workspace: `../setup.sh`
- Windows ARM64 preparation, native build/launch and current limitations: [windows/README.md](windows/README.md)

Below, `$DM_ROOT` is the workspace root directory (default `~/dm-work`): the game files, rewrite build outputs, caches and run logs all live under it and are not committed to the repository.

## How it works

The game program is three NSO files (the Switch executable module format): `main` (the game itself), `sdk` (the Nintendo SDK) and `subsdk0`. They use the same instruction set as the iPhone, so the vast majority of the code can be executed as is; what has to be handled is their dependence on the Switch operating system (Horizon).

### 1. Rewriting the main program (`tools/patch_nso.py`)

Each module is rewritten once, offline, touching only the instructions that cannot be executed as is under the Apple kernel:

| Original instruction | Why it cannot be executed as is | Changed to |
|---|---|---|
| Reads and writes of the `x18` register | The Apple kernel modifies this register | Reads and writes of a thread slot (a per-thread storage location) |
| Reads and writes of `tpidr_el0`, reads of `tpidrro_el0` | The game uses them to find thread-local storage and Horizon's thread-local region; the Apple kernel modifies `tpidr_el0`, and Horizon's thread-local region has no counterpart on Apple platforms | Reads and writes of the corresponding thread slot |
| `svc #n` (system call) | There is no Horizon kernel on Apple platforms | A jump to the "host gate": an entry point into the host's C code |
| Reads of `cntpct_el0` (the system counter) | Horizon's timer has no counterpart on Apple platforms | Also goes through the host gate; the host answers as a 19.2 MHz system counter |

In addition there are two tables registered per program build number, both written in `patch_nso.py`:

- **Cuts** (`CUTS`): targeted modifications of individual instructions or data. Each entry carries the original bytes, the new bytes and a reason; before rewriting, the original bytes are checked entry by entry, and the rewrite aborts on any mismatch.
- **Observation gates** (`OBS`): a gate inserted at the entry of some function that enters a host callback, executes the original instruction and then jumps back, so that the host can read the state at that point.

The rewritten code is never modified again at run time: thread slot numbers are obtained indirectly through a read-only "key page", and gate addresses are fetched from thread slots. The reason is that iOS does not allow executing code written at run time, so the game code must be compiled into the App as a signed code segment.

The output for each module is in the build output directory: `<name>.patched.bin` (the rewritten image, with the key page and the patch area appended), `<name>.fixups.tsv` (layout), `<name>.gates.tsv` (gate table), `<name>.sites.tsv`, `<name>.cuts.tsv`, `<name>.obs.tsv`.

Build outputs have a codename, written as the single line of [PRODUCT](PRODUCT); `tools/product.sh build` uses the same rewriter to produce three kinds of build output in `$DM_ROOT/build/cache/dm/out/<codename>-<kind>`:

| Kind | Ruleset | Where it is used |
|---|---|---|
| `keypage` | `DM_RULESET=native` | `verify/regress.sh` verifies it by default |
| `rules` | `DM_RULESET=touch-run-v4`, without host-private gates (`DM_PRIVATE_GATES=0`) | `verify/rulegate.sh` uses it by default |
| `touch` | `DM_RULESET=touch-run-v4`, with host-private gates | The kind that `ios/run.sh` compiles into the App by default |

### 2. Loading and startup (`host/loader.c`, `host/main.c`)

The host `dmhost` plays the role of Horizon's loader:

1. `loader.c` maps each module's rewritten image into memory, creates the thread slots and fills in the key page, registers every gate according to `<name>.gates.tsv`, performs cross-module relocation, and finally sets segment permissions. The code area is read-only plus executable from the moment it is loaded; after that the loader writes only to data areas.
2. Imported symbols (functions a module requests from outside) are resolved in one of three ways; see the next section.
3. `main.c` initializes the SDK runtime in the SDK's own startup order, runs each module's `init_array` (the table of global constructors), and then enters the game entry point `nnMain`.

Each game thread is a host pthread. `guest_thread_init` in `host/thread.c` fills in those thread slots for the thread: the gate entry, the stand-in for `x18`, the thread-local storage block, and Horizon's thread-local region.

### 3. Taking over system calls and system libraries (`host/hle*.c`)

The game's dependence on the system has three layers, each connected in its own way:

- **Kernel system calls.** A rewritten `svc` goes through `dm_gate` in `host/entry.S`, which saves the registers, and enters `gate_dispatch` in `host/hle.c`, which looks up by the gate's return address which instruction it originally was; system calls are handed to `svc_dispatch`. `hle_svc.c` implements the memory-related calls (it reserves one whole block of guest address space and carves the heap, stack and other regions out of it), and `hle_thread.c` implements the thread and synchronization calls. The SDK's `nn::os` (threads, locks, events and so on) therefore runs as is; only the kernel beneath it is replaced.
- **C library functions.** `hle_table.c` connects the C library functions whose calling convention matches and that do not depend on structure layout directly to Apple's libc.
- **System services.** On a real device the SDK connects to system services such as files, accounts, audio and input through inter-process communication; the host has no such service processes, so it takes over at the SDK interface layer by function name and supplies host implementations directly. Takeovers are registered with two macros in `host/hle.h`: `HLE` fills in imports that no module defines; `HLE_FORCE` is a forced takeover — even if the SDK has a definition, all references resolve to the host implementation. Every such entry must state a reason, and an entry that is registered but referenced by no module aborts at load time.

An import with no implementation at all is connected to a named stub: when called, it prints one line `UNIMPL <function name>` in the log and returns 0.

Online features such as multiplayer, friends and the shop are out of scope: the related interfaces uniformly answer "unavailable", and the game takes its offline branch.

### 4. Graphics layer (`gfx/`)

The game draws with NVN (the Switch graphics interface). `main` imports only one entry that fetches function addresses, `nvnBootstrapLoader`; the host forcibly takes it over, and not a single instruction of the NVN driver in the SDK is executed. `gfx/nvn/hle_nvn.c` records the game's NVN calls as commands, and `gfx/nvn/nvn_mtl.m` replays them as Metal calls. The shaders are not hand-written: the game's original shaders are translated offline into Metal shader libraries by the translation chain under `gfx/tools/` (the last few steps of `../setup.sh`). See [gfx/README.md](gfx/README.md) for details.

### 5. iOS shell (`ios/`)

The App compiles the sources of `host/` and `gfx/` in directly. The front-end interface `host/front.h` is implemented on the Mac by `host/front_mac.m` and on iOS by `ios/Sources/front_ios.m`. The rewritten module images are turned into embedded assembly by `ios/tools/gen_embed.py` and compiled into the executable; the iOS branch of `loader.c` does not copy code, it only maps these blocks back at their original relative positions. Touch controls and the user interface are also in `ios/`. See [ios/README.md](ios/README.md) for details.

## Directories

| Location | Contents |
|---|---|
| `PRODUCT` | The codename of the current rewrite build output, one line |
| `tools/` | The rewriter `patch_nso.py`, the build output script `product.sh`, `dmx.py` for reading the game's resource storage, `casc_overlay.py` for rewriting the resource storage at build time, and a batch of code-reading and analysis scripts |
| `host/` | The host: loading, system call and system library takeover, the Mac front end, in-game features; `host/tests/` holds the unit tests |
| `gfx/` | The graphics layer: `nvn/` (interface layer and Metal replay), `tex/` (texture formats and deswizzling), `tools/` (shader translation chain), `tests/` |
| `ios/` | The iOS App: `Sources/`, the project description `project.yml`, the build-and-install script `run.sh`, `tools/` (asset generation, smoke tests, UI regression), `tests/` |
| `verify/` | Verification: the rewrite-equivalence regression `regress.sh`, the full acceptance run `accept.sh`, and the scripts and format descriptions its items use |
| `data/` | Small data shipped with the repository: the registry of borrowed paths for the resource overlay. The startup prefetch list is not in the public repository: without it there is no prefetch, and startup skips one step of decoding ahead of time |
| `i18n/` | The Chinese-English string table `ui_text.tsv`; `scan_literals.py` checks that all Chinese text the player can see goes through this table, with exceptions registered in `literal_whitelist.tsv` |
| `third_party/` | Third-party code, see [../docs/DEPENDENCIES.md](../docs/DEPENDENCIES.md) |

## Common workflows

**Rewrite the three modules** (already included in `../setup.sh`; rerun after changing `patch_nso.py` or changing the codename):

```
dm/tools/product.sh build
```

To rewrite a single module: `patch_nso.py <NSO file> <output directory> <module name>`.

**Check rewrite equivalence**:

```
dm/verify/regress.sh [build output directory] [module…]
```

It does three things: a structural check (`structck.py`), a before/after equivalence trial of every rewritten site using Unicorn (a CPU emulation library) (`equiv.py`), and host gate functionality (`gatefn`). The default build output directory is `product.sh path keypage`, and the default modules are `main sdk subsdk0`. `--fresh` first produces a fresh output with the current rewriter and then verifies it; `--struct-only` skips the equivalence trial. If any item fails the exit code is 1.

**Build and run the Mac host**:

```
make -C dm/host
cd dm/host
DM_ROMFS=<Update/RomFS directory> DM_SAVE=<save directory> DM_TEMP=<temporary directory> DM_WINDOW=1 \
  ./dmhost "$(../tools/product.sh path touch)" main sdk
```

Usage is `dmhost <build output directory> [module…] [-- game arguments…]`. `DM_SAVE` must be set; without it the host aborts immediately. Common environment switches:

| Switch | Effect |
|---|---|
| `DM_ROMFS` / `DM_SAVE` / `DM_TEMP` | The host directories for the three mount points: game resources, saves, temporary files |
| `DM_WINDOW=1` | Open a window and show the picture (no window if unset) |
| `DM_AUDIO=1` | Actually play sound on the Mac (by default no sound is played, but audio buffers are consumed in real time) |
| `DM_WATCH=<seconds>` | Watchdog: end the process when the time is up |
| `DM_SVC=1` / `DM_IMPORTS=1` / `DM_DBG=1` | Log, respectively, memory-related system calls, import resolution and takeover, and the game's own debug output |

**Unit tests**: `make -C dm/host/tests run`, `make -C dm/gfx/tests run`, `make -C dm/ios/tests test`.

**Build the iOS package**: `cd dm/ios && ./run.sh build`; to install on a device use `./run.sh install`. For signing configuration see the top-level README.

**Full acceptance run**: `dm/verify/accept.sh [--fast|--full]` runs rewrite equivalence, save-file comparison, the screenshot gate, unit tests, the iOS build, functional cases and more together. Besides the game files it also needs reference run data and standard save files, which are not provided with the repository; the emulator driver scripts used to obtain the reference run data are not shipped either, so the full acceptance run cannot be run with this repository alone.

## Files in host/

**Loading and execution**

| File | Responsible for |
|---|---|
| `main.c` | Bootstrap: load modules, initialize the SDK runtime, enter `nnMain` |
| `loader.c` | Multi-module loading: mapping, thread slots, gate registration, cross-module relocation, takeover by name, segment permissions |
| `entry.S` | The common gate entry `dm_gate`, the table of named stubs for unimplemented imports |
| `hle.c` | Import resolution (`resolve_import`) and gate dispatch (`gate_dispatch`) |
| `thread.c` | Thread slot initialization for each game thread |
| `hle.h`, `dm.h` | Common headers: the `HLE` / `HLE_FORCE` registration macros, the module table, logging |

**Kernel and system libraries**

| File | Responsible for |
|---|---|
| `hle_svc.c` | Kernel system calls: address space and heap, queries, debug output, timing |
| `hle_thread.c` | Thread and synchronization system calls (threads, locks, condition variables, events) |
| `hle_guestthread.c` | Game threads started by the host that carry a full SDK thread context |
| `hle_table.c` | The pass-through table of C library functions |
| `hle_os.c` | Common odds and ends: the side table from game objects to host objects, clocks |
| `hle_sdkinit.c` | Takeover of the initialization of each system service at SDK startup |

**System services**

| File | Responsible for |
|---|---|
| `hle_fs.c` | File system: mount names mapped to host directories |
| `hle_tact.c`, `hle_granny.c` | Host implementations of zlib decompression of resource storage blocks and of Granny data section decoding |
| `hle_audio.c`, `hle_codec.c` | Audio output and Opus decoding |
| `hle_hid.c`, `hle_touch.c` | Gamepad and touch-screen input |
| `hle_swkbd.c`, `hle_charname.c` | The system software keyboard connected to a host input box; Chinese allowed in character names |
| `hle_oe.c`, `hle_time.c`, `hle_account.c` | Operating environment, clocks, local user |
| `hle_movie.c` | Cutscene videos: not decoded, no picture shown, played as empty for the original duration |
| `hle_net.c`, `hle_friends.c`, `hle_misc.c` | Networking and friends connected as unavailable; the remaining miscellaneous services |
| `hle_lifecycle.c` | The host's exit flow |

**Front end and diagnostics**

| File | Responsible for |
|---|---|
| `front.h`, `front_mac.m` | The front-end interface and its Mac implementation (window, keyboard) |
| `dm_input.h` | The state the front end writes for the input layer |
| `crash.c` | Crash state printing, watchdog |
| `diag.c` | The probe `DM_PROBE`, the UI message log `DM_UIMSG`, main-thread sampling `DM_SAMPLE` |
| `memstat.m`, `hle_count.c`, `hle_fdstat.c` | Memory accounting, takeover call counts, file descriptor statistics |
| `romfs_validate.c` | Checks the game resource directory at startup |

**In-game features**

The `dm_*.c` files are features and touch adaptations added on top of the original game; the comment at the top of each file explains what it does and which addresses in the game it relies on. For example `dm_cursor.c`, `dm_world.c`, `dm_actions.c` (touch selection, interacting with the world, buying/selling and spending points), `dm_hud.c` (the numbers on the bottom interface), `dm_vault.c` and `dm_vault_ui.c` (the Chronicle vault), `dm_pouch.c` (the pouch), `dm_rift.c` (the rift), `dm_cloudsync.c` (iCloud save sync), `dm_custom.c` (the switches for these features). The addresses in the files are all offsets into the original 3.3.94012 image.

## Generating assets from your game files

The repository contains no images, tables or text derived from the original game files (the widened stash, the pouch frame, the right-hand panel of character select, the minimap goal, the skill atlas, the bottom UI, the Chronicle vault table, the experience table and so on). They are made on the spot at build time from **your own game files**, and the outputs do not go into git.

**Which files are needed**

- Required: the update package RomFS of the Switch release 1.0.34.0 (corresponding to PC version 3.3.94012) (`Update/RomFS`, which contains `data/build_manifest.psv`). If the version is wrong, the generation scripts report an error based on the registered checksums ("原件和冻结的不一样", i.e. "the original differs from the frozen one"; "原文变了", i.e. "the original text has changed").
- Optional: an unpacked directory of the PC version of D2R (containing `hd/global/ui/...`, the raw `.sprite` files). With it, the bottom UI uses the PC version's 2x original images; without it, the bottom UI uses upscaled Switch assets and will be slightly blurry, and generation prints "没找到 PC 版文件……底栏用 Switch 素材放大，会略糊" ("PC version files not found … the bottom bar uses upscaled Switch assets and will be slightly blurry").

**Where to put them**

- Switch RomFS: default `$DM_ROOT/data/switch/3.3.94012/Update/RomFS`, or use the environment variable `DM_ROMFS_SRC=<your Update/RomFS>`.
- PC version unpacked directory: default `$DM_ROOT/data/assets-src/raw`, or `DM_PC_DATA=<directory>`.

**Which command to run**

- Build the iOS package: `cd dm/ios && ./run.sh build`. It automatically runs `tools/gen_assets.py` and the host table header generation at the start; there is no need to run them separately.
- Generate assets only: `python3 dm/ios/tools/gen_assets.py [<Update/RomFS>]`. Nothing is regenerated if the inputs have not changed.
- Mac host: `make -C dm/host`. The three missing generated files (`dm_vault_tab.h`, `dm_hud_exp.h`, `dm_bright_levels.inc`) are generated automatically according to `DM_ROMFS_SRC`.
- iOS unit tests: `make -C dm/ios/tests test`. The missing skill index is generated automatically.

When game files are missing, a one-sentence message says directly which directory to put them in. These commands call `python3` and must be run inside the Python environment created by `setup.sh` (Pillow, numpy and the rest are installed there; for versions see [../docs/DEPENDENCIES.md](../docs/DEPENDENCIES.md)): run `source ~/dm-work/build/cache/dm/venv/bin/activate` first (if you set `DM_ROOT`, use it in place of `~/dm-work`).

**What is produced**

| Output | Location (not in git) | Generation script (under `dm/ios/tools/`) |
|---|---|---|
| Widened stash background, Chronicle page cover | `dm/ios/Gen/assets/NativeHUD/widestash/` | `gen_widestash_art.py`, `gen_vault_cover.py` |
| Pouch frame | `dm/ios/Gen/assets/NativeHUD/pouch/` | `gen_pouch_art.py` |
| Right-hand panel of character select | `dm/ios/Gen/assets/NativeHUD/touch/` | `gen_fe_rightpanel.py` |
| Minimap goal marker | `dm/ios/Gen/assets/NativeHUD/mapgoal/` | `gen_map_goal_art.py` |
| Bottom UI (background and layout) | `dm/ios/Gen/assets/NativeHUD/v4/` | `gen_hud_v4.py` (geometry in `hud_v4_design.py`; layout = original layout + `touchrectv1_patch.json`) |
| Skill atlas, skill index | `dm/ios/Sources/TouchArt/dm_native_*.png`, `dm/ios/Sources/dm_touch_native_art.c` | `native_art/generate.py` (checks the originals against `dm/ios/Sources/TouchArt/native_art_manifest.json`) |
| Chronicle vault table, experience table | `dm/host/dm_vault_tab.h`, `dm/host/dm_hud_exp.h` | `gen_vault_table.py`, `gen_hud_exp.py` (called by `dm/host/Makefile`) |
| Level day/night table (used by 3D picture brightening) | `dm/host/dm_bright_levels.inc` | `dm/tools/gen_bright_levels.py` (also called by `dm/host/Makefile`) |

The original game text used by item annotations is read from the game's string tables at build time; the repository stores none of it. The old-translation table and the one-line-comment table are not shipped with the repository; you can supply your own, in the format described under `dm/ios/tools/annot/`. Without them these two steps are skipped automatically and the rest of the annotations work as usual.

`dm/ios/tools/check_hud_gen.sh` checks that these generated files have not been added back to the repository.
