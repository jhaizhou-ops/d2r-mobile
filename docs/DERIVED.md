# Game-related material in this repository: what is here and what is not

中文: [DERIVED.zh-CN.md](DERIVED.zh-CN.md)

The front page says the project "contains none and distributes none" of the game's content. This page makes that statement precise: what is truly absent, and which game-related records are in the repository, what each one is, and why it has to be there.

## Not in the repository

- The game program (not a single piece of code from the main executable or the system libraries)
- Art, audio, cinematics
- The game's text (the string tables for item names, skill names, dialogue and so on, in any language) and its data tables
- Shaders (the original shader packages, and the Metal source and compiled libraries translated from them)
- Saves

All of these are generated on your own machine from your own game files. Every time the public repository is exported, a tool takes the real game files and searches the whole tree for them byte by byte; a hit blocks the export.

A few things derived from the game files are used internally but are not shipped here: the startup prefetch list (each line carries the first bytes of an original resource block — without it, launch skips one ahead-of-time decode step: measured with the iOS package running on a Mac, the character select screen is reached in 3.3–4.0 s versus 2.8 s with the list, two runs each; not measured on a phone), the seed save for the shared stash (without it, the game creates an empty one itself as the original does), and two fixtures for the interface-binding tests (captured from the memory of the running game — without them, the three tests that read them are skipped automatically).

## In the repository

These are records of facts about the game, not game content: without them, you could not produce a working package even from your own game files.

| Where | Size | What it is | Why it has to be here |
|---|---|---|---|
| `dm/gfx/nvn/shader_map.tsv` | 1.33 MB | The identifier and checksum of every original shader variant, and the correspondences between them. No shader code | Shader translation uses it to tell which shader is which |
| `dm/gfx/nvn/shader_facts.tsv` | 1.13 MB | The interface description of every shader: which texture slots it uses, of what type, and how many outputs | Translation needs these facts; they would otherwise have to be looked up in the PC release's files, and most people only have the Switch release |
| `dm/gfx/nvn/pso_manifest.tsv` | 177 KB | The list of render pipeline state combinations the game uses | Pipelines are built ahead of time at launch, so the first use does not stutter |
| `dm/gfx/nvn/names.txt`, `nvn_slots.tsv` | 16 KB, 24 KB | The 523 function names of the console graphics API, and where the game's main program keeps them | The host takes over these functions by name |
| `dm/gfx/nvn/include/nvn.h` | 89 KB | Declarations of the console graphics API. Third-party work under the MIT license; the author is named in the file header | The graphics layer implements against it |
| `dm/ios/tools/touchrectv1_patch.json` | 28 KB | A description of changes to the original controller interface layout, carrying widget names and values from that layout | The touch interface is derived from the original layout |
| `dm/data/casc_overlay/borrowed_paths.tsv` | 2 KB | The path names of 6 original textures | Our new panels reuse these original textures |
| `dm/verify/logicself/*.json`, `dm/verify/touchcases/tests/fixture/`, `dm/verify/shotgate_pages/` | about 200 KB in total | Verification baselines: per-tick state checksums recorded while running the original, a few bytes of input commands, and comparison specs for interface screenshots | They prove the rewritten program behaves like the original |
| `main+0x…` addresses in the source (about 800 of them, across 59 files) and their descriptions | — | The locations of functions and data structures inside the game's main program, with descriptions of what they do | The host and the touch layer hook into the game at these locations; valid for version 3.3.94012 only |
| The rewrite rules in `dm/tools/patch_nso.py` | — | Each rule reads "original instruction word → replacement instruction word". The original word is there to confirm the version and the location | This is what the rewriter does |
| Mac PC input adapters and their tests (`dm_cursor.c`, `dm_world.c`, `dm_controls_mac.*`, `patch_nso.py`) | — | Version-specific binding record layouts, native call signatures, call-site/preimage guards and cursor dimensions/hotspots; no executable or artwork | Route physical input through original game behavior and reject an incompatible 3.3.94012 ABI; cursor images are extracted privately |
| A small number of the game's internal names in the source | — | For example the internal names of about 70 monsters the Kill Board recognizes, and the keys of items and interface widgets that feature code looks up | Features find their targets by these names |

### About the 7 rewrites for offline play

Seven of the rewrite rules in `dm/tools/patch_nso.py` exist to let the game run without a network connection (the original checks online at startup). **These 7 are taken from an existing community offline fix** (LFS Mod Offline Fix); they are not our original work. We have no reliable record of where it was first published or who wrote it — if you know, please open an issue and we will add the credit. This project is fully offline and has nothing to do with online play.
