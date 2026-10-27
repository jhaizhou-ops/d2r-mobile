# dm/third_party：第三方代码

English: [README.md](README.md)

这里是别人写的代码，随仓库一起提供，不用另外下载。它们各自沿用原来的许可证，许可证文件就在各自目录里。

| 目录 | 是什么 | 版本与改动 | 用在哪 | 许可证文件 |
|---|---|---|---|---|
| `bcdec/` | BC 压缩贴图的解压，单个头文件 `bcdec.h` | 0.985（上游提交 `80859ed`），未改动 | 图形层：显卡不支持 BC 格式时在 CPU 上解压贴图（`dm/gfx/tex/nvn_bcdec.c`） | `bcdec/LICENSE`（MIT 或 Unlicense，任选其一） |
| `flip/` | NVIDIA FLIP：模拟人眼观看的图像差异度量。`FLIP.h` 加命令行工具的源码 `tool/` | 目录里没有记录 | 验证：`dm/verify/gfxq/` 的画质度量，先 `make -C dm/third_party/flip` 编出 `flip` | `flip/LICENSE`（BSD 3-Clause） |
| `libdeflate/` | 解压库 | 上游提交 `92e6a0d`，未改动 | 宿主：游戏资源块的 zlib 解码（`dm/host/hle_tact.c`）。只编解压相关的五个源文件，名单在 `dm/host/Makefile` | `libdeflate/COPYING`（MIT） |
| `spirv-tools/` | SPIRV-Tools 的优化器库，带它引用的 SPIR-V 头文件 | 标签 `vulkan-sdk-1.4.357.0`（v2026.3），未改动；只放库源码，另有预生成的表 `generated/` 和我们写的 `CMakeLists.txt` | 安卓图形层：Adreno 750 这一族上载入着色器时内联辅助函数（`gfx-inline`） | `spirv-tools/LICENSE`（Apache 2.0）、`spirv-tools/spirv-headers/LICENSE` |
| `ryujinx/` | Ryujinx 的着色器翻译库，三个项目：`Ryujinx.Graphics.Shader`、`Ryujinx.Common`、`Spv.Generator` | Ryujinx 1.3.2，未改动 | 着色器翻译工具 `dm/gfx/tools/dmsh_xlate` 引用它，把 Switch 原版着色器翻译成 SPIR-V | `ryujinx/LICENSE.txt`（MIT）；`ryujinx/src/Spv.Generator/LICENSE` 是该子项目自带的许可证文本 |

几点补充：

- `flip/tool/` 里还带着 FLIP 命令行工具用到的三个单头文件库：`stb_image.h`（2.23）、`stb_image_write.h`（1.13）、`tinyexr.h`。它们的许可声明写在各自文件里。
- `bcdec/README.md`、`libdeflate/VERSION.md`、`ryujinx/README.md`、`spirv-tools/README.md` 记着各自的来源与用途。
- 来源地址与许可证的汇总在 [docs/DEPENDENCIES.zh-CN.md](../../docs/DEPENDENCIES.zh-CN.md) 的「随仓库的第三方代码」一节。那一节还列了两处不在本目录的第三方代码：`dm/gfx/nvn/include/nvn.h`（MIT，作者见文件头）和 `dm/gfx/nvn/nvn_tex.c` 里移植自 deko3d 的排布尺寸算法（zlib 许可证）。
