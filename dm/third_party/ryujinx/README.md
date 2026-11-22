# Ryujinx shader translator (vendored subset)

Three projects from Ryujinx 1.3.2, with the local changes listed below: `Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator`.
The shader translation tool in `dm/gfx/tools/dmsh_xlate` references them to translate the original Switch shaders to SPIR-V.
License: MIT, see `LICENSE.txt`. Upstream: <https://github.com/Ryubing>.

Ryujinx 1.3.2 的三个项目，本地改动见下：`Ryujinx.Graphics.Shader`、`Ryujinx.Common`、`Spv.Generator`。
`dm/gfx/tools/dmsh_xlate` 的着色器翻译工具引用它们，把 Switch 原版着色器翻译成 SPIR-V。许可证 MIT，见 `LICENSE.txt`。

## Local changes / 本地改动

Each one is marked `D2R-Mobile` in the source.

- `Ryujinx.Common/Logging/Formatters/DynamicObjectFormatter.cs`: a trimming warning is suppressed for NativeAOT (log lines only).
- `Ryujinx.Graphics.Shader/Decoders/Block.cs`: `Split()` now moves the old successors' predecessor links to the right block. Upstream left them pointing at the left block,
  so the BRX jump-table search could walk a wrong path, find no targets and drop everything after the BRX. In the Paladin's Concentration aura shader that was
  the colour outputs and EXIT, so the aura's ground effect drew nothing (#215).

源码里每处都标了 `D2R-Mobile`。

- `Ryujinx.Common/Logging/Formatters/DynamicObjectFormatter.cs`：为 NativeAOT 压掉一条裁剪警告（只影响日志行）。
- `Ryujinx.Graphics.Shader/Decoders/Block.cs`：`Split()` 把原后继块的前驱指向改到右半块。上游仍指着左半块，BRX 跳转表的回溯会走错路、找不到跳转目标，
  BRX 之后的代码整段丢掉；圣骑士专注光环的像素着色器丢的正是颜色输出和 EXIT，光环的地面效果画不出来（#215）。
