# Cross-difficulty Wirt's Leg prototype (#159)

Status: source-table prototype only. This does NOT enable the feature in the app. No runtime interception, save rewriting or generated game data is included. It is intentionally not connected to gen_romfs_overlay.py until the supported game's native recipe and completion checks are inspected.

`python3 dm/ios/tools/cow_leg_recipe.py LOCAL_cubemain.txt NEW_output.txt`

The tool identifies exactly one enabled Cow Portal output with exactly leg + tbk, numinputs2, no additional output, and op28 with empty/zero param/value. It removes only the op cell, leaving all other bytes, rows, BOM and line endings unchanged. Already blank op is idempotent. Unknown schema/conditions, ambiguous recipes, extra ingredients and malformed rows fail closed. Output must not already exist; the source is never overwritten. This is not a .bin compiler or overlay installer. If the actual 3.3.94012 row differs, STOP and inspect it; do not broaden matching just to make a build pass.

Before activation, maintainer AI must prove that the cow output handler separately enforces current-difficulty Baal completion, camp location and per-run restrictions. If op28 also enforces completion, this simple patch must not be used: separate the conditions instead. Preserve recipe enabled/version/minimum difficulty fields, native item consumption and portal target. Do not alter stat356 globally or relax other quest recipes. Wire the scoped source change into the actual data compilation and all relevant overlay variants; verify the game loads the generated binary, not merely the edited text.

Tests use authored miniature TSV tables only: `python3 -m unittest discover -s dm/ios/tools -p test_cow_leg_recipe.py -v`. They cover single-cell byte preservation, BOM/LF/CRLF, idempotence, reversed ingredients, refusal cases and CLI source/output protection. Literal/i18n and diff checks pass. These do not establish actual game compatibility or eligibility preservation.

Acceptance still required: leg origin3 × current difficulty3 combinations on save copies; all9 should open the current-difficulty portal only after that difficulty is completed. Negative cases: unfinished current difficulty, wrong location, missing leg/tome, held item and an already-open run; follow the supported game's existing behavior and never consume items on rejection. Check other quest recipes, reload and item counts. No game executable/RomFS or native build/device validation is available here. Keep the implementation PR draft; #159 remains open.
