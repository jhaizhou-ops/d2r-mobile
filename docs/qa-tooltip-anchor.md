# Item preview positioning (#48)

Inventory-only previews previously replaced both axes of the native item target with an opposite-side top point. The revised policy keeps the spare side panel (the maintainer's concern about descriptions covering slots), chooses the side nearest the item, and preserves the original target y and height. The game still measures the text and chooses above/below with edge clamping. Stash/vendor/cube/mercenary paths are excluded. `DM_TEST_TIPDOCK=0` is a native-anchor comparison.

The actual OBS_TOOLTIP callback with authored widgets checks four item corners, unchanged non-target registers, and the native control. Normal and UBSan runs pass. This verifies arguments passed to the game, not final rendered geometry.

Native acceptance remains required: on iPad and wide iPhone test short charms, long runewords, equipped gear, comparison text and all inventory corners. Verify no box leaves the screen or covers inventory slots, belt or Store; check stash/vendor/cube/mercenary placement remains correct. Attach real post-change screenshots before accepting the draft. No game executable/RomFS is available here, so no full build or iPad rendering is claimed.
