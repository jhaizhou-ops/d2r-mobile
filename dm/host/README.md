# dm/host: host file index

Optional [Mac PC input profile](../../docs/mac-pc-input.md): integration prerequisites and focused tests.

中文: [README.zh-CN.md](README.zh-CN.md)

The host does two things: it loads the rewritten game program and provides the platform services that program calls. The features added on top of the original game, and the touch adaptation, live in this directory too. For how it all works see [dm/README.md](../README.md).

The file name prefix tells you which part a file belongs to:

- No prefix (`main.c`, `loader.c`, `entry.S`, …) and `hle*.c`: loading, system calls, system services, front end and diagnostics. These are listed one by one under "Files in host/" in [dm/README.md](../README.md); this page does not repeat that and only adds what is missing there.
- `dm_*.c`: features added on top of the original game, and the touch adaptation. That section gives only a few examples; this page lists them all by responsibility.

Every file opens with a comment saying what it does and what that is based on. Addresses in the feature files are all offsets into the original 3.3.94012 image.

## Base files not listed in that section

| File | What it does |
|---|---|
| `Makefile` | Builds the Mac host `dmhost`. `hle_*.c` and the graphics-layer sources are picked up by wildcard, so adding a file needs no change here; the three missing tables (`dm_vault_tab.h`, `dm_hud_exp.h`, `dm_bright_levels.inc`) are generated from your game files and are not in the repository |
| `hle_target.c` | Auto-target priority for touch taps: bosses, then elites, then normal monsters. Hooks the comparison function of the original controller targeting; the original eligibility, range and line-of-sight checks still apply |
| `hle_private_aim.c` | Aiming by dragging a skill button: converts the right stick's on-screen direction into a direction in the game world |
| `touch_mac.c`, `touch_mac.h` | The touch front end of the Mac host: feeds touch points tick by tick from a touch script (`DM_TOUCH_SCRIPT`); the touch cases in [dm/verify](../verify/README.md) run on it |
| `romfs_validate.h` | Interface of `romfs_validate.c`: checks that the resource archives exist and have the right size, and produces the list of what is missing for the player |
| `stash_seed.h` | The preset content used the first time the shared stash is created. Not shipped in the public repository (it is the bytes of a save the game generated): without it nothing is seeded |
| `dm_derived_rt.h` | Used only in the "shell" that contains no game content: the tables generated from game files are not compiled in but read from the install package at run time; see [docs/NO_XCODE.md](../../docs/NO_XCODE.md) |

Also, besides diagnostics, `diag.c` holds the entry hook of every server tick: work that `dm_vault.c`, `dm_pages.c` and others must do inside a server tick is called from there.

## In-game features (`dm_*`)

### Touch and UI interaction

| File | What it does |
|---|---|
| `dm_cursor.c`, `dm_cursor.h` | The layer where touch meets the game UI: turns touch points into the original cursor position and confirm presses, sends taps to the original widgets as UI messages, and decides where a drag-and-drop lands. The per-frame work of the features is also dispatched here, by touch / controller mode |
| `dm_cursor_item.c`, `dm_cursor_item.h` | Which item the cursor is pointing at, taken from the game's own hover object rather than computed back from cell coordinates |
| `dm_world.c`, `dm_world.h` | Tapping a target in the game world interacts with it: tap an item name to pick it up; tap an NPC, object or exit to walk over and interact. Reuses the game's own click entry point |
| `dm_actions.c`, `dm_actions.h` | Entry points for "select first, then confirm": buying and selling at merchants, stat and skill point allocation, setting the quantity box. All through the paths of the game's own controller version |
| `dm_textinput.c` | Connects UI text boxes (gold amount, Chronicle search and so on) to the system keyboard |
| `dm_textbox.c`, `dm_textbox_core.c`, `dm_textbox_core.h` | Makes plain text boxes display the color codes in item annotations correctly; `_core` is pure logic that does not touch game memory and is tested directly by the unit tests |

### Controller and the two modes

| File | What it does |
|---|---|
| `dm_padmode.c` | The two UI layouts, controller and touch: connecting a controller brings back the original controller UI, touching the screen brings back the touch UI, with no restart |
| `dm_modetx.h` | The mode-switch transaction: freeze input, the old side lets go, the game rebuilds its UI, the new side takes over, input is released. A pure state machine |
| `dm_mode_items.h` | Registry of behavior that runs only in touch mode or only in controller mode; a new mode-specific behavior gets one row here |
| `dm_padmap.h` | Maps the four face buttons of a physical controller to game buttons, by the letter printed on the key cap |

### Screen and picture

| File | What it does |
|---|---|
| `dm_uiscreen.c`, `dm_uiscreen.h` | On screens narrower than 16:9 (iPad), lays the UI out at 16:9 while the 3D picture still fills the whole screen |
| `dm_bright.c` | Dynamic exposure for the 3D brightness boost: how much to brighten depends on the kind of level you are in |
| `dm_hud.c`, `dm_hud.h` | The numbers on the bottom UI (level, experience percentage, difficulty), and the request for a difficulty step (1–8) |

### Text and language

| File | What it does |
|---|---|
| `dm_i18n.c`, `dm_i18n.h` | Looks up our own UI text by key; the language follows the game's language setting |
| `dm_i18n_tab.h` | The compile-time table generated from the string table. Generated; do not edit by hand, see [dm/i18n/README.md](../i18n/README.md) |
| `dm_update.c` | The build number and new-version notice on the character select page |
| `dm_namegen.c` | Random names when the name field is left empty: assembled from syllables, avoiding names already taken |

### Endgame modes

| File | What it does |
|---|---|
| `dm_rift.c`, `dm_rift.h` | The Rift: the portal, picking maps, progress, the guardian, the timer and how many floors a clear is worth |
| `dm_rift_ui.c` | The Rift panel, built from original widgets |
| `dm_doorspot.h` | Where the two doors appear when a Rift floor is settled: guaranteed to be on a cell the player can walk to. Pure algorithm |
| `dm_bossrush.c`, `dm_bossrush.h` | Boss rush |
| `dm_elysium.c`, `dm_elysium.h` | Elysium mode: a per-character switch chosen at character creation; on entering a game it changes data tables in memory, and restores the original values on leaving |
| `dm_kills.c` | The kill board: per character and per difficulty, the kill count, first-kill time and fastest time for each boss |

### Stash and items

| File | What it does |
|---|---|
| `dm_vault.c` | The server half of the Chronicle vault: depositing and withdrawing identified unique and set items |
| `dm_vault_ui.c` | The Chronicle vault's UI: the sixth tab of the stash |
| `dm_pages.c` | Personal stash pages 2–5: the items of the other pages are kept in a side file in the character's directory, and page turning happens inside the server tick |
| `dm_pouch.c` | The pouch and "Deposit": runes and gems picked up in the field go into the pouch without taking inventory cells, and are stored away when you open the stash back in town |
| `dm_runeword.c` | The runeword helper: long-press a socketed base to list the runewords it can make, and socket them in one tap once you have the runes |

### Convenience features

| File | What it does |
|---|---|
| `dm_potpick.c` | Picks up matching potions automatically when the belt is not full |
| `dm_beltauto.c` | Refills the belt with potions when the inventory is opened |
| `dm_dropfilter.c` | The light beam and landing sound for high-value drops |
| `dm_drophaptic.c` | Haptic feedback when a high-value item lands, in three strengths |
| `dm_freetp.c` | Free town portal: reuses the original town portal skill directly |
| `dm_townwp.c` | Entering a game, or a quick restart, puts you next to the town waypoint |
| `dm_shoprefresh.c` | "Refresh Stock" on the merchant trade page; calls only the original stock mechanism |

### Settings, saves and small shared pieces

| File | What it does |
|---|---|
| `dm_custom.c`, `dm_custom.h` | The "Options → Custom" page: all feature switches are read and written here; modules only call `dm_custom_get` |
| `dm_cloudsync.c` | The core of iCloud save sync: comparing and moving files between the local save directory and one cloud directory, without touching the iCloud API |
| `dm_unit.h` | A small shared helper: reads which level a unit is in |

## tests/

The host's unit tests, in one command:

```
make -C dm/host/tests run
```

It needs the game files and a workspace prepared by `./setup.sh`. The `t_*.c` files are per-module unit tests. Four source-only checks run with them: no writes to the original game's UI widgets (`native_write_scan.py`), registration of touch / controller mode behavior (`mode_scan.py`), the string table matching its generated header (`dm/ios/tools/dm_i18n.py --check`), and the literal gate (`dm/i18n/scan_literals.py`); so do two checks against the original program image (`movie_cut.py`, `oe_notify.py`).

The directory also holds a few standalone scripts that are not part of that command (`run_rule.py`, `run_manifest.py`, `move_codec.py`, `client_sta_gate.py`, `server42_rule.py`, `server_walk_request.py`); what each is for, and its limits, is in its file header.
