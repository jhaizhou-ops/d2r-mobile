# SPIRV-Tools (third party, Apache 2.0, see LICENSE)

中文在下面。

- Upstream: <https://github.com/KhronosGroup/SPIRV-Tools>, tag `vulkan-sdk-1.4.357.0` (v2026.3); SPIR-V headers from <https://github.com/KhronosGroup/SPIRV-Headers>, the same tag (`spirv-headers/`, MIT-style Khronos license, see `spirv-headers/LICENSE`).
- Use: on the Adreno 750 family the Android engine inlines the helper functions of shaders whose subgroup instructions sit in a called helper, at load time, with the optimizer passes merge-return, inline-entry-points-exhaustive and eliminate-dead-functions (`inline_rewrite` in `dm/gfx/vulkan/engine/xbg_game.cpp`, marker `gfx-inline`). Qualcomm's shader compiler crashes on that shape (#123, #153, #166).
- Built into the Android package only (`dm/android/CMakeLists.txt` adds `dm_spirv_opt` before the engine); other platforms do not use it.
- What is here: `include/`, and from `source/` the top level plus `util/`, `val/` and `opt/`: exactly the 199 source files the upstream CMake compiles for `SPIRV-Tools` and `SPIRV-Tools-opt` on macOS. Not here: tests, tools, examples, `diff/`, `fuzz/`, `link/`, `lint/`, `reduce/`, `wasm/`, `mimalloc.cpp`, the two precompiled-header sources and `util/timer.cpp`. Of SPIRV-Headers, only the 9 headers the sources include.
- `generated/`: the 6 files the upstream CMake generates with Python (instruction tables, generator list, two extended-instruction headers), generated once from the same tag so the build runs no Python; `build-version.inc` is set to a fixed string instead of the build time.
- `CMakeLists.txt` is ours. Upstream code is unchanged.
- Check: inlining the 187 such shaders of the import pack with this library gives the same bytes as `spirv-opt --merge-return --inline-entry-points-exhaustive --eliminate-dead-functions` (187/187).

## 中文

- 上游：<https://github.com/KhronosGroup/SPIRV-Tools> 标签 `vulkan-sdk-1.4.357.0`（v2026.3）；SPIR-V 头文件来自 <https://github.com/KhronosGroup/SPIRV-Headers> 同一标签（`spirv-headers/`，Khronos 的 MIT 式许可，见 `spirv-headers/LICENSE`）。
- 用途：Adreno 750 这一族上，安卓引擎载入「子组指令写在被调用的辅助函数里」的着色器时，用优化器的 merge-return、inline-entry-points-exhaustive、eliminate-dead-functions 三步把辅助函数内联进入口（`dm/gfx/vulkan/engine/xbg_game.cpp` 的 `inline_rewrite`，标记 `gfx-inline`）。高通的着色器编译器碰到这种结构会崩（#123、#153、#166）。
- 只编进安卓包（`dm/android/CMakeLists.txt` 在引擎之前加入 `dm_spirv_opt`），别的平台不用。
- 这里有什么：`include/`，以及 `source/` 的顶层与 `util/`、`val/`、`opt/`——正好是上游 CMake 在 macOS 上为 `SPIRV-Tools` 和 `SPIRV-Tools-opt` 编译的 199 个源文件。没放：测试、工具、示例、`diff/`、`fuzz/`、`link/`、`lint/`、`reduce/`、`wasm/`、`mimalloc.cpp`、两个预编译头源文件、`util/timer.cpp`。SPIRV-Headers 只放源码引用到的 9 个头文件。
- `generated/`：上游 CMake 用 Python 生成的 6 个文件（指令表、生成器名单、两个扩展指令头），用同一标签生成一次放在这里，构建时不跑 Python；`build-version.inc` 改成固定文字，不带生成时间。
- `CMakeLists.txt` 是我们写的。上游代码未改动。
- 核对：用这个库内联导入包里的 187 个这种着色器，结果与 `spirv-opt --merge-return --inline-entry-points-exhaustive --eliminate-dead-functions` 逐字节相同（187/187）。
