# Close X targets

The existing nine-ring snap searched up to 90 layout units (scaled by UI height). It now searches seven additional rings out to 160 units, restricted to `TouchClose`, `close` and `CloseButton`. Existing direct hits, sliders and nearby ordinary controls keep priority; normal controls do not gain extra reach. The same widget hit tests, visibility, panel clipping and layer floor apply. Item-area classification still precedes snapping.

The production `ui_snap` regression uses authored widgets at two resolutions: 16 assertions, six failures before the change, zero after; UBSan also passes. It covers horizontal and vertical misses, maximum range, ordinary buttons and hidden close buttons. No game content is included.

Native validation is pending (no executable/RomFS available): on iPad check inventory, stash, cube, vendor, skills, quests, mercenary, settings and Chronicle. Tap around each X, adjacent tabs, slots, background and modal dialogs. Confirm expanded targets never close a covered panel or steal item/tab gestures; settings must retain its apply/discard confirmation. Check a controller/mouse mode switch. Keep the PR draft until post-change screenshots and device results are attached.
