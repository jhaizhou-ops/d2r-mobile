# Potion auto-pickup boundary (#9)

The existing sender already restricts native healing/mana types (including rejuvenation), belt fit and drop visibility. This adds a separate item-code allowlist: hp1–hp5, mp1–mp5, rvs and rvl only. Unknown codes and stamina, antidote, thawing, scrolls, tomes and throwing potions are rejected before eligibility/send. Ranking remains large purple > small purple > red/blue grades. Normal difficulty and filter-off use the same allowlist. Native manual pickup and difficulty-specific display filters are unchanged.

This is defensive enforcement of the requested boundary, not proof that the reported items came from this sender. Current evidence does not distinguish incidental label taps from other native interactions in the player's Normal run. Do not close #9 based on these tests alone.

Tests call the production code classifier: 70 assertions, normal and UBSan pass, including zero/space-padded codes, all red/blue grades, both purples and rejected codes. They do not exercise the game's packet handling or item-type tables.

Native follow-up (no executable/RomFS available here): in Normal, with auto pickup on/off and drop filtering on/off, place the denied consumables alongside red/blue/purple potions. Separately stand still, walk by with the stick, tap bare ground, cast and tap each label. Test full/empty belts and inventory/cube capacity. Match item ID across the pickup request/source and destination logs; the log label “player tapped” only means absent from the host auto-request table, not proof of physical input. Verify item counts and manual pickup remain correct. Test other difficulties too. No native reproduction or complete repair of the reported symptom is claimed.
