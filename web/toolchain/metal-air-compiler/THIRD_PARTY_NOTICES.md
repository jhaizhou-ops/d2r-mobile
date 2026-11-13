# AIR release notices and corresponding source

This directory contains D2R integration tests, not a duplicate compiler distribution. The repository license does not relicense upstream compiler or SDK components.

An accepted runtime release must include digest-bound original LLVM, Clang, LibFloor, bzip2, tar and Emscripten component notices under `licenses/`, plus an exact `ORIGINS.json` provenance map and GPL-2.0 text. The consumer preserves those bytes; it does not add MIT labels to fork-owned SDK files or infer that one license covers all components.

The corresponding-source contract requires the exact built fork Git tree, build workflow and scripts, installed Emscripten runtime sources (`src/`, `system/lib/`, `system/include/`, `LICENSE`, `AUTHORS`) and separately identified artifact-only notices. Preserve it and the release metadata alongside any compiler redistribution. CI mirrors that exact asset before uploading compiler diagnostics or preview binaries.

Historical attribution and original texts are retained in [pre-fork notices](history/PRE_FORK_THIRD_PARTY_NOTICES.md) and `history/licenses/`; they describe earlier artifacts, not the fresh release's licensing closure. File-specific third-party notices are not removed or relicensed by this migration.
