# MatePad mini: experimental HarmonyOS bring-up report

中文：[MATEPAD_MINI.zh-CN.md](MATEPAD_MINI.zh-CN.md)

This is a report from one physical tablet, tested on 2026-10-24. It **does not establish compatibility of the published shell**. Gameplay was reached using a locally rebuilt, experimental personal package; the complete compatibility changes are not included in this documentation PR.

## Environment and scope

- Huawei MatePad mini, model MLR-AL00; Maleoon 910 GPU.
- HarmonyOS 7.0.0.109 SP6, device-reported API 26, 4 KiB pages.
- Source baseline: v2.0, `b811b90172f4eb40ec1bc89d903308e0c5ab88d0`, with local changes; shell baseline 0.19.6/code125.
- Owned Switch data: 1.0.34.0 / 3.3.94012; runtime generated for `ohos`.
- Debug signing, signature verification and replacement installation succeeded.

## Startup blockers observed before local workarounds

1. Making anonymous writable memory executable failed with `mprotect` returning `EINVAL`. Separate aligned allocations were also tested. This observation alone does not establish the underlying OS policy or explain other devices' behavior.
2. After the executable-memory blocker, shader validation rejected subgroup operations in non-compute stages. Reported subgroup stage support was compute-only (`0x20`).

The personal build loaded locally generated, signed guest ELF libraries through the system linker, checked image size and compared the loaded bytes against the imported runtime before proceeding. Those libraries contain game-derived data and are not distributable shell assets.

An offline audit found 182 non-compute shaders containing subgroup operations. The existing quad rewrite followed by dead-code/capability cleanup handled 176; six needed a separate experimental rewrite. All resulting modules passed SPIR-V validation, but this is **not** a proof of complete visual equivalence. No shader binaries, generated tables, game-specific shader identifiers or personal packages are attached here. These experiments require a separately reviewed implementation before claiming stock support.

## Results of the personal build

Character selection, entering camp, moving, attacking, menus/maps and exploring multiple areas were exercised. Save transaction commits and return to character selection were observed. The user reported multiple manual launches; a proposed automated three-start check was stopped after a screen-lock launch rejection and was not completed. Exact reloaded character progress was not independently checked.

Two performance findings may be useful for separate changes:

- Noncoherent cached readback memory was invalidated over the whole 32 MiB arena allocation for each small view. A local atom-aligned range implementation retained existing fences/barriers and passed an independent device readback check. In one same-process camp comparison, median invalidation time was about 7.9 ms versus 0.04 ms; sampled average FPS was about 23 versus 28. Adaptive pacing, temperature and frequency varied; this is not a general gameplay benchmark.
- Slow terrain streaming included repeated full BC6H environment-cube decoding. A bounded, exact-byte block cache passed 640 host image comparisons against the original decoder, and the first two eligible device uploads also matched full decoding. Pre-reserving the temporary upload vectors was then tested. The user reported improvement, with brief terrain-edge drops remaining. These local experiments are not part of this PR.

The final personal configuration used native UI output, configured 3D scale 0.50 with a GPU-driven floor of 0.46, and a 30 FPS pacing cap. Static scenes could approach 30 FPS. Running into new terrain still dropped into the teens in some trials; longer logs also contained texture-processing and first-use pipeline-creation stalls. Sustained 30 FPS, complete visual correctness and all-scene stability are **not verified**.

## Follow-up boundaries

Keep this device outside the published-shell verified list until the executable-memory and shader compatibility paths are reviewed and reproduced with an upstream build. Readback-range and texture-decoding changes should be reviewed separately, with their own tests and controlled device comparisons. Do not infer tablet compatibility from the marketing OS version alone.

This report intentionally contains no device serial/IP, account information, signing material, raw diagnostic logs, local paths, game content, generated shaders or installation packages.
