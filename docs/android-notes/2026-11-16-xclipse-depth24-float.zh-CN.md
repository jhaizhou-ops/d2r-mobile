# 三星 Xclipse：没有 D24 深度格式，24 位深度改存 32 位浮点（2026-11-16）

English: [2026-11-16-xclipse-depth24-float.md](2026-11-16-xclipse-depth24-float.md)

[#250](https://github.com/jhaizhou-ops/d2r-mobile/issues/250)：三星 Exynos 手机（S22 Ultra / Xclipse 920、A55 / Xclipse 530、S25 FE / Xclipse 940）开游戏黑屏几秒后闪退，日志是 `vkGetPhysicalDeviceImageFormatProperties(...) failed: -11`，调用栈 `get_image_slow` ← `clear_attachment`。

## 原因

- 游戏的深度缓冲是 NVN Z24S8（0x35）和 Z24X8（0x33），引擎原来建成 `VK_FORMAT_D24_UNORM_S8_UINT` 和 `VK_FORMAT_X8_D24_UNORM_PACK32`。
- 三星 Xclipse 驱动（AMD RDNA）这两个格式一项能力都没有：A55 日志的 `NVN caps formats` 行里没有 125 和 129，S25 FE 的 Vulkan Caps Viewer 显示两者 "Optimal tiling: none"。Vulkan 对深度模板附件只保证 D24S8 或 D32S8 之一，这些驱动有 `D32_SFLOAT`（126）和 `D32_SFLOAT_S8_UINT`（130）：附件、采样、深度比较、传输都有（A55：`0xce01`、`0xc601`）。
- 同一批驱动所有深度格式（包括 D16）都不报 `SAMPLED_IMAGE_FILTER_LINEAR`，所以引擎的采样器在那里本来就对深度降成 NEAREST，并记一行 `NVN sampler linear unsupported`。

## 改动

- `choose_depth_storage`（`xbg_game.cpp`，在预热线程开始之前运行）：X8_D24 或 D24S8 缺深度模板附件、采样或传输能力，而 D32F / D32FS8 都有时，Z24X8 存成 `D32_SFLOAT`，Z24S8 存成 `D32_SFLOAT_S8_UINT`。标记文件 `gfx-depth24-float`（1 / 0）或 `DM_GFX_DEPTH24_FLOAT` 可以强制开或关。启动日志：`NVN depth24 Z24X8/Z24S8 stored as ...`。D16 的选择（`depth16_as_d24`）原样挪进同一个函数。
- 着色器从两种格式读到的都是 0..1 的深度。
- 上传（`append_depth_planes`）把每个 24 位定点值 v 换成最接近 v/(2^24−1) 的 float；读回（`copy_texture_buffer`）把 float 换回最接近的定点值（NaN 和负数为 0，大于 1 为 2^24−1）。函数是 `xbg_depth.c` 的 `xbg_depth_unorm24_to_float` / `xbg_depth_float_to_unorm24`，全部 2^24 个值往返无损。
- 预热列表：读入的记录里渲染通道附件格式按同样的规则改写（D24S8 → D32FS8；X8_D24 → D32F，除非 X8_D24 同时是 D16 的替身），别的手机做的种子记录才对得上运行时的键，也不会用驱动没有的格式建渲染通道。`NVN warm list=... depth24-remapped=N` 记改写了几条。
- 深度偏移：浮点深度缓冲的常数偏移单位随图元深度的指数变（深度在 [0.5, 1) 时是 2^-24，和 D24 一样，更小的深度更细）。引擎不改常数，只把替身上前四次带偏移的绘制记进日志（`NVN depth bias on the float stand-in ...`）。

## 验证

- **单测** `dm/gfx/vulkan/engine/tests/depth24_float_test.c`（只用 CPU）：全部 2^24 个值往返无损、取最近的 float、严格递增；float → 定点的边界（NaN、±0、负数、无穷、非规格化数、1 − 2^-24），以及 [0, 1] 里每隔 4099 个 float 的最近取整。gcc 14 `-O2`、gcc 14 `-O0 -fsanitize=undefined`、clang 18 `-O3` 都是 67,628,688 项检查、0 失败：
  `cc -std=c11 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/depth24_float_test.c dm/gfx/vulkan/engine/xbg_depth.c -lm -o /tmp/depth24_float_test && /tmp/depth24_float_test`
- **Redmi K50 Ultra（Adreno 730，有 D24），本分支的调试 APK。** 罗格营地，3D 比例固定 0.75，固定游戏种子，标记 `gfx-depth24-float` 为 0 和为 1 各冷启动一次：
  - 为 1 时启动日志：`stored as D32_SFLOAT/D32_SFLOAT_S8_UINT (marker gfx-depth24-float)`，`NVN warm list=392 enabled=1 depth24-remapped=267`。
  - 两次的预热：建好 392 条、失败 0，命中 178 次，绘制时新建管线 0 条。改写后的记录和运行时的键完全对得上。
  - 两个 Z24S8 目标（2000×900、1500×675）建成 `D32_SFLOAT_S8_UINT`，初始上传走了转换。D16 阴影图仍是 X8_D24。
  - 场景深度（Z24S8）用线性缩小过滤采样，730 上的 D32FS8 也不支持，于是降成 NEAREST，和 Xclipse 上会发生的一样。
  - 替身上没有深度偏移（营地只在 D16 阴影图上用偏移），也没有深度读回，读回的转换只有单测覆盖。
  - 同一场景的截图只在会动的内容上不同（火焰、雨、草木、鸡、血球蓝球、小地图标签）。0.3% 的像素差超过 24/255；墙、人物和阴影边缘一致。
  - 营地 GPU 时间：D24 14.30 ms，D32 替身 14.2 ms。这是两次单独的运行，不是进程内 A/B，只说明看不出代价，不算测出的差别。
- 完整构建：docker（NDK 29）里构建调试 APK，无警告。

## 未覆盖

- 需要在 Xclipse 手机上测（会请 #250 的报告者测）。
- 如果报告者的日志显示替身上用了深度偏移，并且出现阴影粉刺或 z-fighting，可以在有 `VK_EXT_depth_bias_control` 的设备上精确要求 D24 的单位。
