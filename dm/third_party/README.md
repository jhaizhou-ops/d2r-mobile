# dm/third_party: third-party code

中文: [README.zh-CN.md](README.zh-CN.md)

This is code written by others, shipped with the repository so there is nothing to download. Each component keeps its original license, and the license file sits in its own directory.

| Directory | What it is | Version and changes | Where it is used | License file |
|---|---|---|---|---|
| `bcdec/` | Decompression of BC-compressed textures, a single header `bcdec.h` | 0.985 (upstream commit `80859ed`), unmodified | Graphics layer: decompresses textures on the CPU when the GPU does not support BC formats (`dm/gfx/tex/nvn_bcdec.c`) | `bcdec/LICENSE` (MIT or Unlicense, your choice) |
| `flip/` | NVIDIA FLIP, an image difference metric that models human viewing. `FLIP.h` plus the source of the command-line tool in `tool/` | Not recorded in the directory | Verification: the image quality measurements in `dm/verify/gfxq/`; build `flip` first with `make -C dm/third_party/flip` | `flip/LICENSE` (BSD 3-Clause) |
| `libdeflate/` | Decompression library | Upstream commit `92e6a0d`, unmodified | Host: zlib decoding of the game's resource blocks (`dm/host/hle_tact.c`). Only the five decompression-related source files are built; they are listed in `dm/host/Makefile` | `libdeflate/COPYING` (MIT) |
| `spirv-tools/` | SPIRV-Tools optimizer library with the SPIR-V headers it includes | Tag `vulkan-sdk-1.4.357.0` (v2026.3), unmodified; only the library sources, plus pre-generated tables in `generated/` and our own `CMakeLists.txt` | Android graphics layer: on the Adreno 750 family, inlines shader helper functions at load (`gfx-inline`) | `spirv-tools/LICENSE` (Apache 2.0), `spirv-tools/spirv-headers/LICENSE` |
| `ryujinx/` | The shader translation library from Ryujinx, three projects: `Ryujinx.Graphics.Shader`, `Ryujinx.Common`, `Spv.Generator` | Ryujinx 1.3.2, unmodified | Referenced by the shader translation tool `dm/gfx/tools/dmsh_xlate`, which translates the original Switch shaders to SPIR-V | `ryujinx/LICENSE.txt` (MIT); `ryujinx/src/Spv.Generator/LICENSE` is that subproject's own license text |

A few more notes:

- `flip/tool/` also carries three single-header libraries used by the FLIP command-line tool: `stb_image.h` (2.23), `stb_image_write.h` (1.13) and `tinyexr.h`. Their license notices are in the files themselves.
- `bcdec/README.md`, `libdeflate/VERSION.md`, `ryujinx/README.md` and `spirv-tools/README.md` record where each came from and what it is used for.
- Upstream addresses and licenses are summarized under "Third-party code in this repository" in [docs/DEPENDENCIES.md](../../docs/DEPENDENCIES.md). That section also lists two pieces of third-party code outside this directory: `dm/gfx/nvn/include/nvn.h` (MIT, author named in the file header) and the layout size algorithm in `dm/gfx/nvn/nvn_tex.c`, ported from deko3d (zlib license).
