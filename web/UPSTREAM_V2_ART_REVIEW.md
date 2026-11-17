# v2 art source contracts

The production v2 shell profiles select the matching b811 release art and overlay revision. Revision selection is internal, not a user setting; `shell-profiles.mjs` and the recipe source guards bind it to each shell.

- Native art checks both frozen decoded-RGBA and complete encoded-PNG digests. Explicitly pinned browser PNG digests remain paired with their frozen counterparts.
- v2 map-goal recipes include Korean, producing 186 labels and 195 files. The legacy revision retains its five-language, 164-file inventory.
- HUD recipes include Korean vital glyphs for the v2 revision. The full iOS silhouette branch is used; CLI-only arguments and native dynamic-library loading are excluded from browser recipes.
- Table/path transforms retain their bounded source-pinned helper subset, rather than importing native build scripts wholesale.

`toolchain/art-recipes-js/release-source-compatibility.json` records the shared source inputs. Source guards reject drift before runtime assembly. See [art recipes](docs/asset-generation.md), [HUD recipes](docs/asset-generation.md) and [shell profiles](docs/shell-profiles.md) for their APIs and binding rules.
