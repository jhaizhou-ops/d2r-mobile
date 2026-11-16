# Adreno 830：局部尺寸 8×4×1 的计算派发多跑一行工作组；计算着色器加派发范围检查（2026-11-13）

English: [2026-11-13-adreno-830-dispatch-guard.md](2026-11-13-adreno-830-dispatch-guard.md)

[#256](https://github.com/jhaizhou-ops/d2r-mobile/issues/256)：骁龙 8 至尊版（Adreno 830）手机上，画面右上被岩浆照亮的地面出现边缘清晰的错误光照方块，并且闪烁（第四幕火焰之河传送点最明显）。[#261](https://github.com/jhaizhou-ops/d2r-mobile/issues/261)（Adreno 825）报告同一位置的条块。

## GPU 做了什么

- 游戏用一个计算着色器（`b54d192f7c620ded`，局部尺寸 8×4×1，每块一个调用）为每个屏幕块建光源列表，32×20 块派发 (4,5,1)。列表第 0 层是槽 `块号`，第 1 层是槽 `640 + 块号`，在同一个缓冲区里。
- Adreno 830（OnePlus 13，驱动 `0x80320050`）上，局部尺寸恰为 8×4×1、y 方向工作组数为奇数的派发会多跑一行工作组。逐个计数调用的探测着色器在 y = 1、3、5、7、9、25、27 时都多跑，偶数从不多跑，试过的其它局部尺寸（4×4、4×8、8×1、8×2、8×3、8×5、8×8、16×4、32×4、32×1、64×1、8×1×8、4×4×4、8×8×8 等）都准确。Adreno 730（Redmi K50 Ultra）与 Mesa 的 llvmpipe 都只跑所派发的范围。
- 光源列表派发多出来的调用当成第 20–23 行块：它们第 0 层的槽（640–767）正是第 0–3 行的第 1 层槽，于是最上面四行块拿到别处的光源；岩浆的光每帧在变，所以闪烁。
- 证据：用 RenderDoc 抓帧取出的输入单独运行这个着色器，830 上每次第 0–3 行第 1 层的列表都错（每次不同），730 与 llvmpipe 上都对（逐字节相同）；在 llvmpipe 上真的多派发一行，得到的数据与 830 一致。

## 改动

`xbg_spirv_dispatch.cpp`：计算模块载入时，引擎在入口函数最前面加上：

```
if (any(gl_WorkGroupID >= gl_NumWorkGroups) || any(gl_LocalInvocationID >= 局部尺寸) ||
    any(gl_WorkGroupID * 局部尺寸 + gl_LocalInvocationID >= gl_NumWorkGroups * 局部尺寸)) return;
```

范围之内的调用结果不变。高通 Adreno 8xx 上缺省开（`xbg_adreno_dispatch_overruns`），标记文件 `gfx-dispatch-guard`（1 / 0）可强制开关，其它 GPU 用环境变量 `DM_GFX_DISPATCH_GUARD` 打开。两个模块载入器都在其它改写之后做；导入包不变。

**为什么比较位置。** 光源列表着色器里多出来的调用工作组号在范围内、组内号越界（y 为 4–7）。830 的编译器认定组内号总小于局部尺寸，把组内号与字面量尺寸的比较删掉了，所以最初的检查（只有前两项）没有任何作用。上限从缓冲区读的比较，以及上面的位置比较，都保留下来。

**风险。** 位置比较在今天的驱动上保留。以后的驱动也可能把它推理掉（组内号小于尺寸、工作组号小于工作组数），闪烁就会回来，而不会有任何报错。上限从缓冲区读的检查删不掉，但要对所有计算着色器传入派发尺寸（推送常量），会改变计算管线布局的建法（布局键、预热名单）。留待需要时再做。Adreno 8xx 手机更新驱动后，到火焰之河传送点看一眼。

## 验证

- 单元测试：`g++ -std=c++17 -Wall -Wextra dm/gfx/vulkan/engine/tests/dispatch_guard_test.cpp dm/gfx/vulkan/engine/xbg_spirv_dispatch.cpp -o /tmp/dispatch_guard_test && /tmp/dispatch_guard_test`（96 种模块形状；范围内外的每个调用；组内号比较被删掉时 830 多出来的调用；18 种认不得的形式原样保留），以及 `adreno_policy_test.cpp` 新加的检查。两者在 AddressSanitizer 与 UBSan 下也通过。
- 导入包的 26 个计算模块经引擎的载入改写链（64 与 128 道的 warp32、NaN min/max、分量折叠、临时变量转发）后都加上检查，并通过 `spirv-val`（Vulkan 1.1）；其它阶段的 6,905 个模块不动。
- 单独运行光源列表着色器（抓帧取出的输入），OnePlus 13：

| 着色器 | 次数 | 正确 |
|---|---|---|
| 不改 | 5 | 0 |
| 只有前两项 | 20 | 0 |
| 加上位置比较 | 40 | 40，与 730 和 llvmpipe 逐字节相同 |

- OnePlus 13 游戏内：先用 `spv-override` 放入加了检查的着色器，再装调试版：火焰之河传送点不再出现方块；日志 `dispatch_guards=13`，没有留下未加检查的计算模块；60 fps。
