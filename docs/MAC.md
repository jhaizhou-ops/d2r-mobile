# Playing on a Mac (macOS App)

简体中文: [MAC.zh-CN.md](MAC.zh-CN.md)

On an Apple silicon Mac this project can be built into a native macOS App, `D2R Mobile.app`: double-click to start, with keyboard, mouse and gamepad all working. It runs the same game and the same features we added (rifts, the trove, the chronicle, the Custom page and so on), using the original's gamepad interface.

**Up front: these five things have not been operated by a person yet** — keyboard keys, a physical gamepad, the resource-folder dialog, ⌘Q and menu clicks, and how it feels in the hand. Where this page says "verified", it was run unattended: keys injected by a test script, the mouse through a test entry, quitting, full screen and resizing through test switches. If you try it, please open an Issue and tell us how it went.

## Requirements

Apple silicon (Intel Macs are not supported; see "[Which devices are supported](../README.md#which-devices-are-supported)" in the README for every device). Two tiers by system:

| Your system | How to build | Output |
|---|---|---|
| macOS 26 or later | Default: `dm/tools/mac_app.sh` | `D2R Mobile.app` |
| macOS 15 – 25 | Older-API build: `DM_MAC_M3=1 dm/tools/mac_app.sh` | `D2R Mobile M3.app` |

The default build puts the graphics side on a graphics interface that only exists from macOS 26 on (Metal 4); on earlier systems it does not open (the system says the App cannot be used with this version of macOS). The older-API build is the same source and the same game with the graphics side using interfaces that older systems also have; its minimum is macOS 15.0. Details: [OLDER_SYSTEMS.md](OLDER_SYSTEMS.md).

## Build

The prerequisites are the same as for iOS: game files in place and `./setup.sh` finished (see "Getting started" in the README). Then:

```bash
source ~/dm-work/build/cache/dm/venv/bin/activate
dm/tools/mac_app.sh                  # macOS 26 or later
DM_MAC_M3=1 dm/tools/mac_app.sh      # macOS 15 – 25
```

The default build takes ten-odd seconds and produces `~/dm-work/out/macapp/D2R Mobile.app` (about 3 MB). The older-API build recompiles the shader libraries for macOS 15 the first time, which adds about 3–4 minutes, and produces `D2R Mobile M3.app`; it has its own save and log directories (with M3 in the name), so it can sit next to the default App. To put the output elsewhere, add an output directory after the command.

The App contains only our own program. The rewritten game program, the shader libraries and the UI overlay are all made from your game files; they stay in your working directory (`~/dm-work`) and the App only records where they are — so **the App runs only on the Mac that built it**. Please do not copy it to anyone, and do not delete or move the working directory (the shader libraries recompiled for the older-API build are in the output directory). Signing is local ad-hoc signing: no developer account and no device registration.

Version and build number can be set: `DM_MAC_VERSION` (default 1.0) and `DM_MAC_BUILD` (default: the current month-day-hour-minute).

The App gets the project's own icon (`dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png`). Use your own picture with `DM_MAC_ICON_SOURCE=<image>` (an opaque square that fills the whole picture; macOS 26 and later round the corners themselves, and a picture with transparent margins is shown shrunk inside a gray frame), or build without an icon with `DM_MAC_ICON_SOURCE=none`.

## First launch

Double-click the App. The first launch asks you to choose the **game resource folder**: pick `~/dm-work/data/switch/3.3.94012/Update` (or the `RomFS` inside it); it must contain a `data` folder. Choosing the folder that directly holds the `.data` files and `build_manifest.psv` also works. A wrong choice is explained and you are asked again; a correct one is remembered.

A key reference is then shown once; afterwards it is under Help in the menu bar.

Language: the first launch follows the system language (Chinese systems start in Chinese, Spanish in Spanish, Russian in Russian, everything else in English). To change it: Options → Custom, the language switch in the first row → Accept All → close and reopen the App.

## Keyboard ↔ gamepad

With a keyboard, the button prompts on screen (the hint bar at the bottom, skill slots, the ends of tab rows) show the keyboard keys directly: J K U I Q E Z C = Tab and so on; press a button on a connected gamepad and they switch back to gamepad letters. The full mapping:

| What you want / gamepad button | Keyboard |
|---|---|
| Confirm (A) | Space or J |
| Back (B) | Esc or K (in game with no panel open, Esc opens the inventory group of pages) |
| Return | confirm; acts as + when the bottom of the page shows a "+" hint (such as "Accept" after naming); on the main menu Return = enter the game |
| + (menu / new character) | the = key |
| − (map) | Tab |
| Left stick (move) | W A S D |
| D-pad | Arrow keys |
| X / Y | U / I |
| L / R | Q / E |
| ZL / ZR | Z / C |
| Left stick press / right stick press | F / G |

**Creating a character, start to finish**: on the main menu press `=` (or click "New Character") → choose a class with the arrow keys and press Return (or click that class's figure) → on the mode page press Return or Space → on the naming page press Return to open the input box, type, Return → press Return once more to create.

A gamepad works as soon as it is connected, alongside the keyboard. The menu bar has View → Full Screen (⌃⌘F) and Quit (⌘Q). The window can be resized freely. In game the picture fills the window or screen at its own aspect ratio (on a 16:10 MacBook both windowed and full screen are filled, with the interface along the bottom, centred while a large panel is open); the front-end pages such as the main menu and character select stay 16:9 with bars above and below.

**Picture settings** are at the top of the first Options tab, under **Display** and **Graphics**:
- **Display Mode** (Windowed / Fullscreen) switches as soon as you press Apply, and the App opens that way next time. Switching with View → Full Screen (⌃⌘F) updates the setting too.
- **Resolution** (1600 × 900 / 1920 × 1080 / 2560 × 1440) is the size the game renders at; on a screen narrower than 16:9 the height follows the screen. It takes effect the next time the App starts, and the page says so until then.
- **Upscaling** (Standard / MetalFX) is how the picture is enlarged to the window or screen: Standard is a plain stretch, MetalFX is Apple's sharper upscaler (about 1.5 ms of GPU time per frame on an M-series Mac). It changes as soon as you press Apply.
- **Graphic Presets**: *Recommended* is what the App picked for this Mac the first time it started: full screen, the highest resolution up to three quarters of the screen's width (2560 × 1440 only on Pro, Max and Ultra chips), and MetalFX when the picture is enlarged. *Performance* is 1600 × 900 and *Quality* 2560 × 1440, both with MetalFX. Choosing a preset fills in the rows; changing a row shows which preset matches, or *Custom*.
- **Default** at the bottom of the panel puts every row back to Recommended (and the Custom rows to their defaults); press Apply to keep it.
- The settings live in `sw_video.txt` next to the App's saves. Delete it to have the App work out Recommended again.

The mouse pointer hides itself after it has rested over the game window for 3 seconds (handy when playing with the keyboard or a gamepad) and comes back the moment the mouse moves. It never hides over the menu bar, other windows or a dialog. To change the delay: `defaults write org.d2rmobile.mac CursorHideSeconds -float 5` (seconds; `0` turns it off; the older-API build uses `org.d2rmobile.mac.m3`), then restart the App.

## Mouse

What it can do now:

- **Click empty ground to walk there**: a left click on empty ground walks the character to that point, finding its way around obstacles; a click inside a wall or water walks to the nearest reachable spot.
- **Hold to keep walking**: hold the left button and the character keeps walking towards the pointer, following it as it moves. W A S D / the gamepad stick take priority: press a direction key while walking and the keyboard takes over at once.
- **Click a monster = attack**: a left click on a monster attacks it with whatever is bound to the main attack key (J) — a melee character walks over and hits, spells and ranged attacks are used on it directly. Holding keeps attacking and chasing; when it is not a monster under the pointer, holding walks towards it as before. Right click is still back.
- **Click a target in the world** (NPC, chest, waypoint, item on the ground): the character walks over and interacts. With a panel open, a click outside it only closes the panel.
- **Items take the mouse now**: a click selects and shows the description; a double click = use / put on / take off (a potion in the belt = drink, the cube = open, a vendor's goods = buy); press and drag = pick up, release = put down (move between cells, equip, into the stash / cube, into the inventory = buy, onto the vendor's shelf = sell), with the item image right on the pointer; press and hold on an empty cell = tidy this page.
- **Click the bottom row**: a skill icon = use that skill (the same as pressing the key shown under it), a potion cell = drink it, the middle button = open the inventory group of pages, the run / walk button = toggle.
- **Click the interface**: buttons, checkboxes and tabs in panels (including the in-page tabs of the skill tree), the pause menu, the settings pages, and "Start" and the close X on the rift level-selection page.
- **Click a key hint at the bottom**: clicking an entry presses that key.
- **Click a figure when creating a character**: the class you click is selected.
- **Top-right minimap**: opens automatically on entering a game; Tab toggles it; clicking the area name at its top switches to the full-screen map, and another click returns.
- Right click = back (B); the wheel scrolls lists.

What it cannot do yet:

- Click-to-walk was verified only in the camp, on empty ground and the waypoint; complex terrain in the wild and clicking NPCs are unverified.
- Switching input device (keyboard ↔ gamepad) while the settings page or the stash is open: the prompts change only after the panel closes.
- A **long press** on an item (more details / take one) is press-and-hold with the mouse; these are the same gestures as on touch.
- "Hold …" key hints need longer than a click, so hold the key on the keyboard; a hint with two key caps is not recognised.
- The life orb, mana orb and experience bar do nothing when clicked.

## Saves, logs and iCloud

- Saves: `~/Library/Application Support/D2R Mobile/save`
- Log: `~/Library/Logs/D2R Mobile/host.log` (the previous run is `host.prev.log`). When reporting a problem use Help → Share Log… in the menu bar: it packs these two logs and a version-information file into one archive and lets you choose where to save it; nothing is uploaded, no saves are included, and the user name in the logs is replaced with `~`.
- No sound: at launch the log has a line `audio：输出设备「…」…，系统缺省输出「…」…` (the output device in use and the system default output; a mismatch is marked red), and on exit a line `声卡回调累计 N` — N = 0 means no sound was played this time. (These log lines are in Chinese.)
- The older-API build (`D2R Mobile M3.app`) uses a separate pair of directories with M3 in the name.

**iCloud save sync: off by default in an App you build from this repository; a paid developer account can turn it on.** The program has the same sync as on iPhone / iPad wired in, but it needs the App to carry the iCloud signing entitlement. By default the App is signed locally without it, so saves stay on this Mac, the log records one line saying iCloud is unavailable and only local saves are used, and the game runs as usual. With a paid developer account, download a Mac provisioning profile that carries iCloud, put the `.provisionprofile` into `~/dm-work/mac-sign` (or point `DM_MAC_SIGN_DIR` at its folder) and build with `DM_MAC_ICLOUD=1 dm/tools/mac_app.sh`: the script reads the team, bundle identifier and entitlements from the profile and signs with the matching certificate in your keychain. The bundle identifier in the profile decides the iCloud container, so a profile for your iOS App's identifier syncs with your iPhone / iPad. Contributed by [@xielh96](https://github.com/xielh96) ([#121](https://github.com/jhaizhou-ops/d2r-mobile/pull/121)). Without a paid account, bring your phone's progress over by importing it by hand as below.

### Importing saves from an iPhone / iPad

Importing is a one-time manual copy: afterwards each side goes its own way and progress diverges; to bring them together later you copy the whole thing again and let one side overwrite the other.

**1. Find the save on the iPhone / iPad**

- Quit the game completely on the phone / tablet first (swipe it away in the app switcher).
- Open the built-in Files app → "On My iPhone" (or "On My iPad") → "D2R Mobile" → the `save` folder inside is the save.

**2. Copy the whole `save` folder, do not pick files**

It holds one folder per character (folder name = character name), a few shared-stash files and a `Settings` folder (game settings including the interface language — after copying, the Mac's game settings become the phone's). The data of the features we added (the extra personal stash pages, the pouch, rift records, the chronicle vault and so on) is in this folder too. The switches on the Custom page are not in it; each device keeps its own.

AirDrop, a cable (the Files tab in Finder) or iCloud Drive all work for getting it to the Mac.

**3. Put it on the Mac**

1. Quit the game on the Mac completely (⌘Q).
2. In Finder press ⇧⌘G, enter `~/Library/Application Support/D2R Mobile` and press Return. (This folder exists only after the game has been started once.)
3. **Back up first**: rename the existing `save` folder there (for example `save backup`); do not delete it.
4. Put the `save` folder from the phone here. Correct looks like this: directly under `D2R Mobile/save/` are the character folders, `Settings` and a few `.d2i` files — no extra level (not `save/save/…`).
5. If `save.work` or `save.new` sit next to it, leave them; the game discards and rebuilds them at launch.
6. Open the game; the phone's characters should appear on the main menu.

Got it wrong or want to go back: quit the game, take the `save` you just put in away, and rename the one from step 3 back to `save`.

The other direction (Mac → iPhone / iPad) uses the same files and mirrors the steps: quit the game on the Mac, copy the Mac's `save` into the game's folder in Files on the phone, replacing the one there (rename the phone's original first as a backup). Keep the App on both sides as current as you can.

Verified: on a Mac, a save placed in the save directory laid out as in step 4 shows its characters on the main menu after launch and can enter a game (one unattended run). The whole route from a real iPhone / iPad through the Files app onto the Mac, the other direction, and hardcore characters have not been operated by a person.

## Quitting and saving

Closing the window, ⌘Q and Quit in the menu all save before quitting, in any state: in game, with the pause menu open, with a panel open, on the loading screen and with the rift panel open were each verified with one run. It waits at most 3 seconds; if saving has not finished by then, the log records a line saying it quit without saving.

After a forced quit or a crash, the system's "reopen windows?" dialog no longer comes up to block the game window on the next launch.

## Known shortcomings

- The five things from the top have not been operated by a person: keyboard keys, a physical gamepad, the resource-folder dialog, ⌘Q and menu clicks, how it feels in the hand. The key mapping was written from the key-code table.
- Click-to-walk was verified only in the camp. Clicking a distant monster through a wall may stop the character at the obstacle; step around and click again. Clicking monsters with a ranged class, and the trove's auto-placement under mouse drags, are unverified. There is no right-click skill as on PC.
- The Share Log menu item and save panel have not been operated by a person; what was verified is the archive it produces.
- "Prompts switch back to gamepad letters when a gamepad is used" was verified by simulation; no physical gamepad has been connected.
- The free town portal on ZR + Space of a gamepad is not there.
- The quest bar on the left in rifts is not there.
- An App you build yourself does no update check (it does not go online), and has iCloud save sync only when built with `DM_MAC_ICLOUD=1` and a paid account's profile (see "Saves, logs and iCloud").
- The older-API build was played on macOS 15 by one player in our test group (Mac mini M4, macOS 15.7, using a package built elsewhere); building it from this repository on a Mac running macOS 15 has not been operated by a person.
