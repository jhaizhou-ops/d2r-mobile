# 片元阶段不支持子组的驱动：片元 shuffle 按放宽模式改写，检查挪到改写之后（2026-11-16）

English: [2026-11-16-fragment-subgroup-fallback.md](2026-11-16-fragment-subgroup-fallback.md)

[#243](https://github.com/jhaizhou-ops/d2r-mobile/issues/243) 和 [#155](https://github.com/jhaizhou-ops/d2r-mobile/issues/155)：`VkPhysicalDeviceSubgroupProperties::supportedStages` 只有计算阶段的驱动（`NVN caps subgroup ... stages=0x20`）上，游戏在主菜单就中止，报 `Actual subgroup operation unsupported in shader stage`。见于 Adreno 650 的旧驱动（骁龙 865 / 870：Retroid Pocket 5、真我 GT Neo 2；Vulkan 1.1.128）和 Maleoon 910（Pura 70 Ultra、Mate X5）。原因是 @kieusonlam 在 #155 里分析出来的。

## 着色器里有什么

用引擎自己的改写代码统计导入包（4,559 个片元着色器）：
- 182 个片元着色器用了子组指令，每个都经过翻译器的一个辅助函数（SHFL.BFLY：对一个 float 做 `OpGroupNonUniformShuffle`，配 `SubgroupLocalInvocationId`）。顶点着色器一个都没有；有 5 个计算着色器用，这些驱动在计算阶段是支持的。
- 现有的 quad 改写（`gfx-quad-derivatives`，安卓上除 Adreno 740–799 一族外缺省打开）把辅助函数的*调用*换成精细导数：278 个里换了 271 个。
- 换完之后 176 个着色器已经调不到 shuffle，但没人调用的辅助函数、lane 内置变量和能力声明还留在模块里。6 个着色器仍然调用它：5 个调用的结果另有按位用途；还有一个着色器的 2 个调用，传进去的值保守分析证明不了是数值（经计算下标读出来的值做了 `FAdd`）。
- `validate_capabilities` 在运行时载入里排在所有改写之前。

## 改动

- `xbg_spirv_quad_derivatives(..., relaxed)`：只在片元阶段不支持子组的驱动上用。
  - 结果另有按位用途的调用，以及值的直接来源是浮点运算或浮点读取的调用，也照样按浮点改写。
  - 值来自 `OpConvertFToS` / `OpConvertFToU` 的调用：转回浮点、取邻居、再转回整数，|v| < 2^24 时精确。D2R 里没有这种，单测覆盖了。
  - 分叉控制流里的调用和别的来源仍然跳过；主函数里直接的 shuffle 不留 shuffle 回退。
- `xbg_spirv_strip_subgroup`：去掉入口点调不到的函数、没人读的子组内置输入变量（连同注解和入口点接口里的条目），以及在没有任何子组指令时的 `GroupNonUniform*` 能力声明。不依赖别的库，鸿蒙构建里也有。
- `quad_rewrite` 在这类驱动上不管 `gfx-quad-derivatives` 怎么设都做这两步；运行时载入在改写之后才查能力，并接受 `DerivativeControl`。
- 测试开关：标记 `gfx-no-fragment-subgroups`（1）或 `DM_GFX_NO_FRAGMENT_SUBGROUPS=1`，把片元阶段那一位清掉。

## 验证

- **单测** `dm/gfx/vulkan/engine/tests/spirv_quad_test.cpp`：52 项，其中 17 项是新加的。
  - 放宽模式：已证明的调用与缺省模式逐字相同；按位用途和整数来源的调用都得到邻居的精确值；分叉控制流里的调用仍然跳过。
  - 清理：去掉辅助函数、lane 内置变量（连同注解和接口条目）和能力声明后结果不变；辅助函数还被调用时什么都不删。
  - clang 18 通过，gcc 14 加 ASan + UBSan 也通过。导出的样例除原有那个故意写错 scope 的拒绝样例外都过 spirv-val。
  - 命令：`clang++ -std=c++17 -UNDEBUG -I dm/gfx/vulkan/engine dm/gfx/vulkan/engine/tests/spirv_quad_test.cpp dm/gfx/vulkan/engine/xbg_spirv_quad.cpp -o /tmp/quad_test && /tmp/quad_test`
- **整个导入包，离线**，用引擎自己的改写代码（含游戏内容，只在本地）：
  - 放宽模式加清理：182 个带子组指令的片元着色器全部清干净，没有子组指令、lane 内置变量和 `GroupNonUniform*` 能力。共改写 278 个调用：271 个和现在一样，7 个按浮点放宽。182 个全部通过 `spirv-val --target-env vulkan1.1`。
  - 缺省模式不变：它改写的 176 个模块与 main 的输出逐字节相同。
  - 挪后的检查对每一族的载入改写链各跑一遍，每次 6,931 个着色器：
    - 安卓缺省（quad、warp32、NaN min/max、常量折叠、临时变量前推）；
    - Adreno 750–799（对 187 个带子组辅助函数的模块做 SPIRV-Tools 内联，再加整理）；
    - Adreno 740（只整理）；
    - 放宽模式；
    - 没有安卓开关的鸿蒙。

    每一种都是 0 个不过。
- **Redmi K50 Ultra（Adreno 730），本分支的调试 APK**，3D 比例固定 0.75，固定游戏种子：
  - 营地，开关关：原来的路径（`fragment subgroups=1 stages=0x30`）。
  - 营地，开关开（`stages=0x20`）：不中止。带 shuffle 的着色器载入时 `stripped=4`，包括放宽的那个（`00f1902d880682c7`，`relaxed_float=1`）。两次都是：预热建好 392 条、失败 0，命中 178 次，游戏中新建管线 0 条。截图只在会动的内容上不同（0.2% 的像素差超过 24/255）。GPU 14.23 对 14.17 ms（两次单独运行）。
  - 罪恶洞穴（引擎自带的传送），开关开：不中止，游戏中经检查载入 118 个着色器，命中 189 次，游戏中新建 0 条，洞穴和光照都正常。
- 完整构建：docker（NDK 29）里构建调试 APK，无警告。

## 未覆盖

- 这里没有 Adreno 650 和 Maleoon 910，Redmi 只能假装不支持。
- Adreno 650：流体计算着色器 `098a126f92bf533a` 在那里仍然建不起来（`VK_ERROR_UNKNOWN`，#155），烟和火会显示成乱码方块。另外改。
- Maleoon 910：#249 说过了检查之后，画人物时驱动里崩溃。这里不处理。

## 后续（2026-11-17）：真 Adreno 650 上内联的先后

@kieusonlam 在 Retroid Pocket 5（原厂驱动，`stages=0x20`，#243）上测了 #280，`gfx-inline` 设成哪个值都启动不了：
- `gfx-inline=1`：辅助函数在 quad 改写之前就被内联了。shuffle 进了入口函数、落在分叉的控制流里，改写一个也换不掉，检查又中止。
- `gfx-inline=0`：流体计算着色器 `098a126f92bf533a` 让 `libllvm-qgl.so` 崩溃（addr 0x48），和 750 一族是同一个编译器问题。

改动：
- `front_rewrites`：片元阶段不支持子组的驱动上，先做 quad 改写和清理，再内联。支持的驱动上顺序不变。
- `xbg_adreno_inline_default`：内联缺省在 750–799 一族和片元阶段不支持子组的高通驱动上打开；标记 `gfx-inline` 仍然优先。

验证：
- `tests/adreno_policy_test.cpp` 通过，新增 7 项。
- 离线按 Adreno 650 的链（片元：放宽 quad 加清理，再内联；计算：内联、warp32、整理）跑 6,931 个着色器，0 个过不了检查。没有片元着色器还需要内联；187 个用到子组的模块全部通过 spirv-val，包括先内联再做 warp32 的 5 个计算着色器。
- Redmi（Adreno 730）营地，开关开（因此缺省内联）：不中止。17 个片元着色器在内联前改写（`stripped=4`），一个都没被内联；`098a126f92bf533a` 和 `6a7a308d61928982` 先内联再做 warp32。预热建好 392 条、失败 0，命中 178 次，游戏中新建 0 条，GPU 14.34 ms。开关关：内联照旧不开，14.38 ms。截图只在会动的内容上不同。
