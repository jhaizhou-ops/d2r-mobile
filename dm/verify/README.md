# dm/verify — Verification Tools

中文：[README.zh-CN.md](README.zh-CN.md)

D2R Mobile rewrites the Switch version of the game program and executes it directly on iOS. This directory answers two questions: does the rewritten program behave the same as the original, and has any given change broken something that already worked.

A few terms used throughout this directory:

- **Build output**: the output directory of the rewriter `dm/tools/patch_nso.py` for the three modules `main`, `sdk` and `subsdk0`, located at `$DM_ROOT/build/cache/dm/out/<codename>-<kind>`; there are three kinds: `keypage`, `rules` and `touch`. `dm/tools/product.sh build` produces all of them in one go, and `product.sh path <kind>` prints the path.
- **Host**: `dmhost`, built from `dm/host`, which loads the build output on a Mac and executes the game. Most verification runs against it.
- **Ground truth**: reference data obtained by running the **original** game in a patched Ryujinx emulator — screenshots, per-tick state, command packets, saves. The host's results are compared against it.
- **Tick**: one step of the game's server-side logic. Many criteria are "identical tick by tick".
- **Run directory**: the directory holding all the output of one run (logs, screenshots, record files).
- `$DM_ROOT`: the workspace root, default `~/dm-work`. Game files, build outputs, ground-truth data and run artifacts all live under it and do not go into the repository.

## Read this table first: what you need for what you run

| What you need | What you can run |
|---|---|
| Repository + Python environment | The script self-tests `touchcases/test_lib.py` and `test_accept.py`: their inputs are all synthetic data or stand-ins; they start no host and read no game files |
| Game files + build output (prepared by `./setup.sh`) | `regress.sh` (rewrite-equivalence regression; also needs llvm, see "External tools" below), `fnfuzz.py`, and the host and graphics-layer unit tests (`make -C dm/host/tests run`, `make -C dm/gfx/tests run`) |
| Plus a standard save | `touchchain.sh` (touch feature test cases) |
| Plus ground-truth data | `shotgate.sh` (screenshot gate), `rulegate.sh`, `logicself.sh`, `opmode.sh`, `dm/tools/host_savecmp.sh`, `dm/tools/save_txn_regress.sh`, `gfxq/`, and `accept.sh`, which chains them together |

**The ground-truth data is not in the repository and never will be**: it is a recording of the original game running. The emulator driver scripts used to obtain it (building the emulator, starting a run, driving and recording) are not shipped with the repository either, so the gates in the last row, which depend on ground truth, cannot be run with this repository alone. This page still keeps their descriptions and the ground-truth formats, because the whole verification system rests on ground truth; see "Where the ground truth comes from" and "Data you have to prepare yourself" below.

External tools:

- The Python packages `lz4`, `capstone`, `unicorn`, `numpy` and `Pillow`, installed in `$DM_ROOT/build/cache/dm/venv` (created by `./setup.sh`). Most scripts use this environment's interpreter by default (`DM_PY` overrides it); a few scripts and the `make` targets call `python3` directly, so run `source ~/dm-work/build/cache/dm/venv/bin/activate` first (if you set `DM_ROOT`, use it in place of `~/dm-work`).
- `structck.py` uses `llvm-objdump` and `llvm-objcopy` as an independent disassembler; their directory is given by `LLVM_BIN`, default `/opt/homebrew/opt/llvm@23/bin`. `brew install llvm` does not necessarily install version 23; if that directory does not exist, set `LLVM_BIN=$(brew --prefix llvm)/bin`.
- Several scripts use the `timeout` command; if your system does not have it you have to install it yourself (it is part of GNU coreutils).

## Rewrite-equivalence regression: `regress.sh`

Verifies that "the rewrite has not changed the program's behavior". Needs only the game files and the build output, and finishes in seconds.

```
dm/verify/regress.sh [--fresh] [--struct-only] [build output dir] [module…]
```

By default it verifies `main sdk subsdk0` of the `keypage` build output. It runs three things in order:

| Tool | What it verifies |
|---|---|
| `structck.py` | **Structural check**. Executes no code and checks the rewriter's output word by word: apart from registered cuts, non-code segments are byte-for-byte identical to the original image; the words changed in the code segment are exactly the registered rewrite sites; an independent disassembler recounts the instructions that should be changed, with none missed and none extra; the patch area no longer contains the original instructions that cannot be executed; the bytes of every host gate, argument gate and observation gate equal the template and match the gate table one to one. The full list of checks is in the file header |
| `equiv.py` | **Site-by-site equivalence**: using Unicorn (a CPU emulation library), for each rewrite site, executes the "original instruction" and the "patch block" separately from multiple sets of random state, and compares all general-purpose and vector registers, flags, stack pointer, memory writes, end address and memory-access faults. Includes a mutation self-check: a batch of patch blocks is deliberately broken, and every one of them must be judged non-equivalent |
| `gatefn/` | **Gate function**: builds a test bench on the spot from the host's current `dm/host/entry.S`, executes each gate one by one, and checks the register write-back convention |

- `--struct-only`: runs only the structural check and the gate function check, not `equiv.py`.
- `--fresh`: first regenerates a copy with this worktree's rewriter into `$DM_ROOT/run/verify/dm/fresh/` and verifies that, and also compares it file by file with the given directory; used to detect whether the shared build output is stale.
- Environment variables: `DM_PY`, `DM_NSO_DIR` (directory holding the original modules, default `$DM_ROOT/data/switch/3.3.94012/Update/ExeFS`), `SEED`, `JOBS`, `LLVM_BIN`.
- Output: a summary table at the end, with one row each for 「结构」 (structure) and 「等价」 (equivalence) per module, including the denominator and the coverage (how much was actually covered); the last line is `REGRESS OK|FAIL <build output name>`, and any failure gives exit code 1. The log is at `$DM_ROOT/run/verify/dm/regress_<timestamp>.log`.

Related standalone tools:

- `fnfuzz.py <build output dir> <module> <NSO>`: function-level differential fuzzing. For each function containing a rewrite site, the original image and the rewritten image each execute the whole function on the same random input, and the final states are compared. Slower than `equiv.py`, not part of `regress.sh`, run on its own as needed; see the file header for options.
- `keypage_check.py`: a tool that checks key-page indirect accesses; see the file header for usage.
- `nsoimg.py`: a common module that unpacks an NSO into a memory image, shared by the tools above.

## Windows ARM64: standalone equivalence

`equiv.py` also accepts Windows **TARGET 3** outputs. Prepare your own 3.3.94012 modules with `DM_TARGET=windows`, then run from PowerShell using a Python environment with `lz4`, `capstone` and `unicorn`:

```powershell
python .\dm\verify\equiv.py C:\dm-work\windows\out main C:\games\Update\ExeFS\main --jobs 4 --tsv C:\dm-work\windows\main.equiv.tsv
python .\dm\verify\equiv.py C:\dm-work\windows\out sdk C:\games\Update\ExeFS\sdk --jobs 4 --tsv C:\dm-work\windows\sdk.equiv.tsv
```

The Windows model keeps live x18 pointing at a synthetic TEB, stores guest x18 in a direct TLS slot at `TEB + 0x1480 + slot*8`, and writes byte offsets into the key page. It independently checks TEB-pointer preservation, including faulting trials, and excludes only registered TLS slots from memory-write comparison, not the entire TEB. Below-SP reads fail, as on Android. Windows Python workers use `spawn`; other systems retain `fork`.

This verifies rewrite-site semantics, both directions of exclusive-window branches and the deliberate mutations. It does **not** execute host-gate semantics or intentional cuts. The Windows CMake tests cover native gate/TLS/stack behavior separately. `structck.py`, `regress.sh`, the ground-truth gates and the full acceptance suite are not ported/accepted on Windows; a successful standalone run must not be reported as `REGRESS OK`. See [Windows preparation and launch](../windows/README.md).

## Host acceptance: `accept.sh`

Runs all the checks in parallel; if any one is red, the whole thing is red.

```
dm/verify/accept.sh [--fast|--full] [build output dir]
```

| Item | Content | Criterion | Prerequisite |
|---|---|---|---|
| ⓪ | `bootchk.sh`: the boot block list (the list of resource blocks pre-read at startup) | The build output directory's `bootlist.tsv` exists, its build number matches `main.cuts.tsv`, it is byte-for-byte identical to `dm/data/bootlist_<build number>.tsv`, its checksums are self-consistent, and it matches the resource pack block by block. The public repository does not ship this list: without it there is simply no prefetch (startup skips one step of decoding ahead of time), but this item reports red; to use it, record one yourself with `dm/tools/tact_bootlist.py`, put it at `dm/data/bootlist_<build number>.tsv` and regenerate the build output | Game files, the boot block list |
| ① | `regress.sh` verifies the build output under test; then `--struct-only` is run on the `touch` build output (the kind compiled into the App) | Two `REGRESS OK` lines | Game files |
| ② | `dm/tools/host_savecmp.sh`: the host replays the input of one ground-truth run, saves and exits, and the save is compared byte by byte with the ground truth's save (`savecmp.py`) | The 6-slot state matches tick by tick; the saves match; the negative control (ground-truth save versus the starting save) must not match; no red lines in the host log | Ground-truth run |
| ②B/C | `rulegate.sh 400` and `logicself.sh <touch build output> 230` | See the next section | Ground-truth run |
| ③ | `shotgate.sh`: the [screenshot gate](screenshot_gate.md) | Every page is green against its ground-truth reference image | Ground-truth reference images |
| ④ | Unit tests: `make -C dm/host/tests run`, `make -C dm/gfx/tests run`, `make -C dm/ios/tests test`, `test_accept.py`, and the two self-tests that belong to the emulator driver scripts (not shipped with the repository) | All six suites pass, with no build or link errors | — |
| ⑤ | Runs ② again under `DM_TEST_LAG=400`: an artificial sleep at the entry of tick 400 creates a "catch-up tick backlog" | The frozen tick still lands on the expected tick; the save criterion is the same as ②; and it is confirmed that the backlog was actually created | Ground-truth run |
| ⑥ | `dm/tools/save_txn_regress.sh`: in a replay run, injects "process killed" and "write failed" by save write count, and checks the committed save directory. Runs only in the full tier | All 4 cases hit; the committed files equal the expected version; the checksums are self-consistent; the negative control holds | Ground-truth run |
| ⑧ | iOS build, compile only without installing (`DM_NOINSTALL=1 dm/ios/run.sh build`) | The build succeeds | Xcode and the signing configuration of `dm/ios` |
| ⑨ | `touchchain.sh` runs all the cases under `touchcases/` | Every case's assertions pass | Standard save |
| ⑩ | `opmode.sh`: after entering a game, switches the operation mode from docked to handheld | A game had been entered before the switch; the game takes two mode-change notifications; the process survives to the time limit without crashing; at least 300 more frames are produced after the switch; the window size is correct; no red lines in the host log | Ground-truth run |

(There is no ⑦ in the list; the numbering follows the script.)

Tiers and scope:

- `--full` (default): the screenshot gate runs all pages, plus ⑥. `--fast`: the screenshot gate samples only the five pages `camp_e stash wild charselect skilltree`, and ⑥ is not run.
- Automatically runs less according to the scope of the change: when the changes are only in `dm/ios` (except source files compiled together with the host) and documentation, only ⓪ ⑧ run; when only in `dm/gfx`, `dm/ios` and documentation, ⓪ ③ ④ ⑧ ⑩ run; in all other cases everything runs. "Changes" means this worktree's committed changes relative to its fork point from the `main` branch plus uncommitted changes; when that branch does not exist, everything runs. Skipped items are listed one by one as 「跳过」 (skipped), not silently. `ACCEPT_ALL=1` forces everything to run.
- Only one instance runs at any moment: the lock is at `$DM_ROOT/run/verify/dm/accept.lock`, and later ones queue.

Output: one line of readings per item, and at the end a 「期望 / 真结果 / 用时」 (expected / actual result / time taken) comparison table and an `ACCEPT OK|RED` line; exit code 0 = all green. Logs are in `$DM_ROOT/run/verify/dm/accept/<timestamp>_<worktree name>_…/`; the directory of a red run keeps a `KEEP` file.

| Environment variable | Effect |
|---|---|
| `TRUTH_RUN` | The ground-truth run directory used by ②⑤⑩, default `$DM_ROOT/data/switch/truth_runs/cvd5` (`cvd5` is just the directory name hard-coded in the script; it has no other meaning) |
| `ACCEPT_ALL=1` | Do not omit anything by change scope |
| `ACCEPT_SCOPE_ONLY=1` | Print only the scope decision line and exit; together with `ACCEPT_SCOPE_CH=<path list>` you can pass the changed paths directly to try it |
| `FUNC_JOBS` | How many cases ⑨ runs at once, default 2 |
| `FUNC_LIMIT_S` | The hang-guard timeout in seconds for each case of ⑨, default 60 |
| `DM_TOUCH_OUT`, `DM_RULE_OUT`, `DM_LOGICSELF_OUT` | The build output directories used by ①, ②B and ②C |

`test_accept.py` is the wiring test of `accept.sh` itself: it substitutes stand-in scripts for the items and verifies only the wiring of the fast / full tiers and failure propagation; it does not run the real host.

### The two logic gates

- `rulegate.sh [ticks=400] [build output=rules]`: the host replays the input of one ground-truth run using the `rules` build output, and the 6-slot state is compared tick by tick with the ground truth's `ticks.jsonl` (the comparison script `selfcmp.py` belongs to the emulator driver scripts and is not shipped with the repository). The ground-truth run directory is given by `DM_RULE_TRUTH` and must contain `meta.json`, `c2s.jsonl` and `ticks.jsonl`. Before starting, it first checks whether the build output was produced with the current `patch_nso.py`, and refuses to run if it is stale. The last line is `RULEGATE OK|RED`.
- `logicself.sh [build output=touch] [ticks=230]`: the build output is compared with **its own baseline**. It replays the same ground-truth run input and compares the per-tick unit counts and hashes with `logicself/touch.json` in the repository, to detect "something that should not affect logic was changed, yet the logic changed". The ground-truth run directory is given by `DM_LOGICSELF_TRUTH` (default `$DM_ROOT/data/switch/truth_runs/cvd5`). `LOGICSELF_REBASE=1` re-records the baseline: it runs twice in a row and writes only if the two runs agree. The last line is `LOGICSELF OK|RED`.

Each of the three record file formats has its own page: [tick_state_format.md](tick_state_format.md), [command_packet_format.md](command_packet_format.md), [ui_message_stream.md](ui_message_stream.md).

The internal branch-merging workflow script is not shipped with the repository; outside contributions do not need it.

## Screenshot gate: `shotgate.sh`

The key screens drawn by the host are compared page by page with the ground-truth reference images; there are currently 17 pages. The criteria, the page spec, and how to produce reference images are all in [screenshot_gate.md](screenshot_gate.md).

Companion tools: `hostshot.sh` (starts one host and takes one screenshot), `shotgate.py` (compares one page), `dynmask.py` (makes animation masks), `shotgate_gc.sh` (cleans up old run artifacts), `logred.sh` (the keyword table of host-log red lines, shared by the screenshot gate and `accept.sh`).

## Touch feature test cases: `touchcases/` and `touchchain.sh`

Each case is a pair of files under `touchcases/<name>/`:

- `script.txt`: the touch-point script (at which moment, which finger, pressed or lifted where), handed to the host's touch front end via `DM_TOUCH_SCRIPT`.
- `assert.py`: the assertions. It reads the four records the host writes in the run directory — per-tick state `ticks.jsonl`, command packets `c2s.jsonl`, UI messages `uimsg.jsonl`, state probes `state.jsonl` — and judges whether this touch produced the result it should (whether an item moved, whether a certain command packet was sent, whether a panel opened). The common functions are in `touchcases/lib.py`, whose own tests are `touchcases/test_lib.py`.

Optional files: `prefix` (one line, the path of a prefix directory; this segment of ground-truth input is replayed first, taking the character to a location such as the wilderness before starting), `ticks` (one line, the total tick count for this case), `pending` (a case with this file is not run but is listed; the first line gives the reason).

```
dm/verify/touchchain.sh <case name>
```

- Cases run on the same UI overlay as the iOS package (`dm/ios/tools/gen_romfs_overlay.py` generates it on the spot from the game resources), so what is tested is the layout the player actually sees.
- The criteria are all logical results, so the run does not follow the product's pacing: rendering is not rate-limited, the game clock is fast-forwarded in fixed steps, and the touch-point script advances by server ticks, so the result is independent of the machine's load.
- The last line is `TOUCHCHAIN <用例> OK|RED 用时 N 秒` (`TOUCHCHAIN <case> OK|RED took N seconds`). The run directory of a passing case is deleted; `DM_TOUCH_KEEP=1` keeps it; red ones are kept in `$DM_ROOT/run/verify/dm/touchchain/`.
- Environment variables: `DM_HOST_OUT` (build output, default `touch`), `DM_TOUCH_SAVE` (the `0/` directory of the starting save), `DM_TOUCH_PREFIX`, `DM_TOUCH_RUNROOT`, `DM_ROMFS_SRC`, `DM_TICKS_N`.

The prerequisite is a standard save (see below). The existing assertions were written against the content of that save — which inventory cells hold items, which skills the character has learned — so with a different save the assertions have to be changed accordingly. Cases with a `prefix` (currently `skill_main`) also need the corresponding ground-truth prefix directory.

## Other tools

| File | Purpose |
|---|---|
| `savecmp.py` | Byte-by-byte comparison of two save directories. Checksum fields are each checked for self-consistency, the two wall-clock time fields are checked for their source, and everything else must be identical |
| `savesha.sh` | Computes the content fingerprint of a save directory |
| `uicmp.py`, `c2scmp.py` | Comparison of UI message streams and command packet sequences; the rules are on their respective format pages |
| `oracle/swgdb.py`, `oracle/swtick.py` | A minimal client for the emulator's GDB interface; per-tick state is read through it |
| `bootlist_static.py` | Static check of the boot block list, called by `bootchk.sh` |
| `gfxq/` | Image quality metric and visual-equivalence ruler, see [gfxq/image_quality.md](gfxq/image_quality.md) |
| `overlay_contract.json` | A draft contract for acceptance of the iOS touch overlay controls; no script reads it at present |

## Where the ground truth comes from

The ground truth comes from Ryujinx (an open-source Switch emulator) running the original game. Stock Ryujinx is not enough and needs a few patches: so that it can directly load an unpacked game directory, can be fed key presses and take screenshots from a script, and can read out the game's internal state at specified locations.

The scripts that build this emulator, start a run, and drive and record it are not shipped with the repository, so ground truth cannot be recorded with this repository alone, and the gates that depend on it cannot be run either. What remains in the repository is the minimal reading side, `oracle/swgdb.py` and `oracle/swtick.py` (a client for the emulator's GDB interface), and the descriptions of the three record formats.

How recording works: through the emulator's GDB interface, the driver script sets breakpoints at the game's UI message dispatch point and the server tick entry, and advances by the game's own messages: every step is "wait for the precondition message → send input → wait for the confirmation message", with no guessing by seconds. The game-creation seed and game-creation time are pinned to the same values on both sides; the host-side counterparts are `DM_GAME_SEED` and `DM_FIXED_TIME`. The `main` the emulator runs carries the cuts registered by the rewriter (except those marked 「仅宿主」 (host only)), so the ground truth and the host run a program with the same targeted modifications.

Recording one run yields a "ground-truth run" directory containing: `uimsg.jsonl`, `ticks.jsonl`, `c2s.jsonl` (the three kinds of record; the formats are on their respective pages), `events.jsonl` (the event stream of the driving process), `meta.json` (seed, time, starting-save fingerprint, etc.), `save_after/` (the save at the end), `shots/` (screenshots).

## Data you have to prepare yourself

| Data | Where scripts look for it | Used by | How to get it |
|---|---|---|---|
| Standard save | `$DM_ROOT/data/switch/save_ref_warlock/0000000000000001/` (containing subdirectories such as `0/`) | Almost every check that runs the host, and the ground-truth emulator | Your own save. `dm/tools/host_savecmp.sh` also identifies the save by `$DM_ROOT/data/switch/<save name>/MANIFEST.sha256`: each line is "sha256 + two spaces + `0000000000000001/relative path`", sorted by path |
| One baseline ground-truth run | `$DM_ROOT/data/switch/truth_runs/cvd5/` | ②⑤⑥⑩ of `accept.sh`, `logicself.sh` | Recorded with the emulator driver scripts (not shipped with the repository). Needs `meta.json`, `c2s.jsonl`, `ticks.jsonl`, `events.jsonl`, `save_after/0`. The specific parameters used to record this run were not kept in the repository |
| The rule gate's ground-truth run | Given by `DM_RULE_TRUTH` | `rulegate.sh` | Same as above; needs `meta.json`, `c2s.jsonl`, `ticks.jsonl` |
| Screenshot gate reference images, animation masks, and the replay files of each page | The paths written in the page specs, all under `$DM_ROOT/data/switch/` | `shotgate.sh`, `gfxq/` | See [screenshot_gate.md](screenshot_gate.md) |
| Prefix directories of touch test cases | The path written in the case's `prefix` file | `touchchain.sh skill_main` | A segment of ground-truth input: `manifest.json` plus `c2s.jsonl` |
| Replay file for the image quality metric's combat frames | Path hard-coded in `gfxq/shoot.sh` and `gfxq/ruler.sh` | `gfxq/` | The `c2s.jsonl` of a ground-truth run that includes combat |

A ground-truth run's `meta.json` records the fingerprint of the starting save, and the scripts that replay it check it; so once the standard save is changed, the ground-truth runs and reference images all have to be re-recorded with the new save.
