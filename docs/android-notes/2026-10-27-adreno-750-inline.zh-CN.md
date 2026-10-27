# Adreno 750：子组指令写在辅助函数里的着色器载入时内联；建管线兜底扩到图形管线（2026-10-27）

English: [2026-10-27-adreno-750-inline.md](2026-10-27-adreno-750-inline.md)

[#123](https://github.com/jhaizhou-ops/d2r-mobile/issues/123)、[#153](https://github.com/jhaizhou-ops/d2r-mobile/issues/153)、[#166](https://github.com/jhaizhou-ops/d2r-mobile/issues/166) 与一份私下反馈（三星，驱动 `0x802fa029`）是同一件事：骁龙 8 Gen 3（Adreno 750）上，v2.0.1 仍在主菜单前后闪退。

## 手机日志说明的事

- 崩溃都在高通驱动自带的着色器编译器 `libllvm-qgl.so` 里：`sig=11 addr=0x48`，返回地址在库内偏移 `0x2ec048`，崩点 `0x463e84`（小米 14、Redmi K80）或 `0x463ed4`（三星；驱动版本不同，同一个函数）。
- v2.0.1 只跳过了计算着色器 `098a126f92bf533a`。#166 里，跳过它之后，下一个计算着色器 `6a7a308d61928982` 照样崩，之后还有三个三万多字的像素着色器也崩。把它们的辅助函数内联后，能进营地。
- 三星那份日志有三次启动，每次载入 45–47 个着色器。其中「子组指令在被调用的辅助函数里」的像素着色器（`358197ac3db59cbc`、`5c1ca4e7c3e4657c`）载入 3 次，3 次都在同步建图形管线时崩；其余 42–44 个都编过，包括一个 36515 字、但没有这种结构的像素着色器。所以触发条件是这种结构，与着色器大小无关。
- 全包里有这种结构的着色器共 187 个（计算 5、像素 182），以上崩过的 8 个全在其中。

## 改动

1. **引擎载入时内联**（`xbg_game.cpp` 的 `inline_rewrite`）：Adreno 750–799（高通）上，载入「子组指令写在被调用的辅助函数里」的着色器时，先用 SPIRV-Tools 的 merge-return、inline-entry-points-exhaustive、eliminate-dead-functions 三步把辅助函数内联进入口，再交给驱动。导入包不变，玩家只装新 APK。原着色器在别的机型上照旧（像素着色器的 quad 改写只认辅助函数调用的形式）。标记文件 `files/dm/gfx-inline`（`1` / `0`）可显式开关；740 只关改写、不内联。启动日志多一行 `NVN shaders with subgroup helpers are inlined at load=…`，每个着色器的 `loaded` 行多一项 `inlined=`。
2. **SPIRV-Tools 随仓库**（`dm/third_party/spirv-tools`，v2026.3，只放库源码与预生成的表，见其 README）：只编进安卓包；鸿蒙等其它平台的引擎没有这一步。库编译时不带调试信息（安卓包保留 .so 的调试信息，带上会让 APK 过 200 MB）；APK 从 23 MB 到 34 MB。旧的试验开关 `DM_VK_INLINE_CS` 删掉了。
3. **建管线兜底扩到图形管线**：以前只管计算管线，而且记号只有一个文件。现在三条建管线的路径（游戏图形线程、预热线程、异步线程）都在建之前把这条管线写进各自线程的 `gfx-pipe-inflight.<槽>`，建完删掉。连着两次启动都在场的才记进 `gfx-pipe-skip`；驱动直接报错的当场记下。条目格式：计算管线写 `c:` 加着色器键，图形管线写 `g:` 加各阶段键。v2.0.1 记下的旧条目（没有前缀，是在不内联时记的）读到就作废，日志 `NVN pipelines skipped by an older version dropped=…`；真还崩会重新记。v2.0.1 那条「750 上直接跳过 `098a126f92bf533a`」的规则删掉了：内联后它能编（#123 在 S24 上验过），流体特效回来；万一还崩，由兜底接住。跳过的是这一种组合的绘制，不连累共用同一顶点着色器的其它绘制。图形管线报错不再让整个 App 退出。
4. **日志**：安卓上图形引擎的日志从标签 `dm-vulkan` 并进 `dm`，同时写进日志文件，`adb logcat -s dm` 一条命令就能拿全。崩溃行里没有符号的库改打「库名+偏移」（例如 `libllvm-qgl.so+0x463e84`），不同手机之间可以直接比。

## 验证

- 现场内联与离线一致：用 `dm/third_party/spirv-tools` 编出的库对导入包里 187 个这种着色器内联，结果与 `spirv-opt --merge-return --inline-entry-points-exhaustive --eliminate-dead-functions` 逐字节相同（187/187）。导入包不变（`dmsh_vk.py` 只删了试验开关）。
- 单测：`g++ -std=c++17 -Wall -Wextra dm/gfx/vulkan/engine/tests/adreno_policy_test.cpp -o /tmp/adreno_policy_test && /tmp/adreno_policy_test`。
- 真机（vivo，骁龙 8 Elite / Adreno 830，驱动 `0x80320049`，调试包，导入包与 v2.0.1 生成的相同）：手上没有 750，这里验的是：内联后的着色器在高通编译器上编得过、画面不变；兜底的记、跳、恢复、旧条目作废都通；830 缺省不受影响。750 不再崩，要报告人实测。

| 局 | 设置 | 期望 | 结果 |
|---|---|---|---|
| A | 缺省 | 不内联，进局，0 崩 | 进局，60 帧，0 崩；97 个着色器无一内联 |
| B | `gfx-inline=1` | 只内联 187 个里载入到的，进局，0 崩 | 载入 97–98 个，其中属于 187 个的 10 个全部内联、其余 0 个；进局，60 帧，0 崩；营地截图与 A 平均差 1.19、99 分位 19 |
| F | `gfx-inline=1`、`gfx-shader-reuse=2` | 预热线程与图形线程各自内联的结果逐字相同 | 不一致 0 |
| G | v2.0.1 的旧跳过名单（`098a`、`6a7a`）＋`gfx-inline=1` | 旧条目作废，两个着色器恢复 | `dropped=2`；两个都内联后正常派发；0 崩 |
| C | 预置两次启动都在建 `g:54f730a6c0efc118+358197ac3db59cbc` | 启动即记下，主菜单画到它时跳过，不崩 | 照期望；进局，60 帧 |
| E | 只预置上一次 | 只记嫌疑、不跳过 | 只生成 `gfx-pipe-suspect` |
| D | 全部清掉 | 恢复 | 照期望 |

载入开销：预热线程已处理过的着色器，图形线程上 0.1–0.3 ms；不在预热清单里的两个计算着色器每次启动第一次载入时内联（830 上 `6a7a308d61928982` 约 30 ms、`098a126f92bf533a` 约 8 ms，在主菜单加载期间）。
