# dm/tools: tool index

中文: [README.zh-CN.md](README.zh-CN.md)

Three kinds of things live here. The first you will use when following the front page or the docs; the second is the measurement and regression scaffolding we use during development, published as is; the third is a set of one-off probes written when the game program was first analysed, kept for the record.

## What you will use

| File | What it does |
|---|---|
| `patch_nso.py` | The program rewriter: rewrites the few instructions in the game program that cannot run unchanged under Apple's kernel and leaves everything else byte-for-byte intact. Called by `setup.sh` |
| `extract_game.sh` | Extracts the two parts this project needs (program files and resources) from your own game package into the workspace |
| `d2rm_pack.py` | Produces a playable install package without Xcode by building your own game files into the "shell"; see [docs/NO_XCODE.md](../../docs/NO_XCODE.md) |
| `build_deswizzle.py` | Builds the generators' native texture DLL on Windows x64 from this repository's C sources using LLVM / the NDK; no game files, Visual Studio or Windows SDK needed. See [docs/ANDROID.md](../../docs/ANDROID.md) |
| `guest_dylib.py` | Writes the rewritten game program as an Apple-format dynamic library (pure Python, byte-identical output on every system); called by `d2rm_pack.py` |
| `winlf/` | Makes the packaging tool chain use `\n` line endings on Windows, so packages made on different systems match |
| `product.sh` | The identifier of the current rewrite output lives in one place (`dm/PRODUCT`); scripts get paths through this |
| `dmx.py` | A minimal read-only reader for the game's resource storage: list files, fetch a file |
| `casc_overlay.py` | Builds the overlay at build time: points chosen resource paths at our new content without touching the original archives |
| `astc_pack.py` | Supplement pack for older devices: converts BC-format game textures offline into ASTC, which those devices support |
| `gen_bright_levels.py` | Generates the level table used by the 3D brightness boost (built from your game files) |
| `tact_bootlist.py` | Generates the startup prefetch list: resource blocks needed before character select, decoded ahead of time at launch |

## Measurement and regression scaffolding

| File | What it does |
|---|---|
| `host_savecmp.sh` | Byte-for-byte save comparison: the host replays a game, saves, and the save is compared with a reference |
| `save_txn_regress.sh` | Determinism regression for save transactions |
| `host_war13.sh` | Host replay of one fixed case, compared tick by tick and by save |
| `l3reg.py`, `l3seg.sh`, `l3run.sh`, `test_l3reg.py` | Long-run memory regression: fits the growth of memory and handles against game ticks and extrapolates to a long session |
| `frameprobe.sh` | Comparison measurement of frame pacing stability |
| `conc_watch.sh` | Concurrency check for timed runs: a timing taken while other heavy work was running is discarded |
| `sampagg.py`, `lockalign.py` | Aggregation of sampling logs; call-chain statistics for lock-holding threads |
| `hle_inventory.py` | Inventory of the host's system service implementations (full / simplified / stub) |
| `sdis.py`, `sideeff.py` | Disassemble one function; walk the call graph for a function's side effects |
| `blackcols.py` | Checks a screenshot for fully black columns (whether the picture fills the screen) |
| `guirun.sh` | Starts a program inside the graphical session and collects its output |
| `mini_sync.sh` | Syncs the workspace to a second Mac for heavy runs (the target is set in the local config) |

## One-off probes from the early analysis

`nso.py` (splits the game program into segments), `fn.py` (function boundaries from the unwind table), `sweep.py` (linear instruction sweep), `x18.py` and `cat18.py` (find and classify instructions using x18, a register Apple reserves), `xref_sdk.py` (cross-references of system library functions), `tp.c` (checks how the thread pointer register behaves under Apple's kernel). They were written before the rewrite approach was settled and depend on intermediate files from that time; they are not guaranteed to run as they are. The basis of the rewrite approach is in [dm/README.md](../README.md) and [dm/verify/README.md](../verify/README.md).
