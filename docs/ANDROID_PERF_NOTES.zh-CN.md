# Android 性能核对清单（上游经验 × 仓库实测）

> **这份文档是什么（人话）**：合并前"旧代码快照"时代写的一份核对清单——当时把上游（iPhone 版）
> 的性能经验逐条对照我们的仓库，列出"该做什么、优先级多高"。
> **现在还有用吗**：基本只剩历史价值。清单里的事**到现在基本都做完了**（管线缓存落盘、带宽、
> 翻译线程、测量工具……），而且当时引用的文件路径和行号是旧结构、早已对不上。
> **别照这份干活。** 所有条目的最新结论都在 `docs/android-perf-notes.md`（大白话版）；踩坑规矩在
> `docs/android-perf-lessons.zh-CN.md`。

> **状态：核对清单 v1 —— 未动任何代码。** 来源 = 上游来信（2026-10-16，iOS 实测经验）+ 5 路并发深度解剖（分块带宽 / 管线缓存 / 翻译线程 / 测量工具链 / 现成收益）。
> 相关文档：[VK_RENDER_PROGRESS.zh-CN.md](VK_RENDER_PROGRESS.zh-CN.md)（实测日志）、[VK_CORRECTNESS_PLAN.zh-CN.md](VK_CORRECTNESS_PLAN.zh-CN.md)（正确性）、[ANDROID.zh-CN.md](ANDROID.zh-CN.md)（总体计划）、[ANDROID_UI_PARITY.zh-CN.md](ANDROID_UI_PARITY.zh-CN.md)（UI 对齐，独立工作线）。
> **基线说明**：本文行号以本仓快照 `android-ours @ 0149751` 为准。你在跑 60fps 实验的树若领先于此快照，实施前先复核对应函数（尤其 `nvn_vk.c`）。
> 所有"建议开关"遵循仓库既有风格（`DM_VK_*` env，启动时打印，便于 adb 核验）。

---

## 0. 结论速览

| # | 项目 | 现状（实测） | 动作 | 优先级 |
|---|------|------------|------|--------|
| 1 | **VkPipelineCache** | **零命中**（三处创建全传 `VK_NULL_HANDLE`） | 落盘缓存 + 预热清单 + 后台串行编译 | ★★★ 最大低垂果实 |
| 2 | **分块带宽** | 仅 stencil 有 DONT_CARE；color/depth 全 `STORE` | 按 §2 机会点逐个打开（先低风险两项） | ★★★ |
| 3 | **测量基建** | 有分项计时/累计计数/`@display_ms` 行；无热翻转开关、无配对统计 | §5 最小协议：热翻转 + 掉帧口径 + 配对脚本 | ★★★ 先有尺子 |
| 4 | **翻译线程化** | 回放层已实现（真实回放链，非存根）；无翻译线程 | rebase 白拿共享层 + 两类适配（§4） | ★★ |
| 5 | **3D 比例自动降档** | 本树为**旧版**简单控制器（2 档） | rebase 拿到新版（5 档+滞回），Vulkan 接线渲染比例 | ★★ |
| 6 | **显存池按需驻留** | 无对应物（上游改的是 Metal 层） | 在 `nvn_vk.c` 池路径做"按需绑定"（实施时定机制） | ★ |
| 7 | **翻译/渲染微项** | — | WaitSync nop、hz_auto、p5/p50/p95 间隔日志等随 rebase 到 | 白拿 |

**测量口径整包采纳上游**（§5）：同进程 A/B 交替 + 配对差 + 符号检验；热机稳态只取尾 30s；掉帧按上屏间隔（60 档 >25ms）；游戏内存判场景；校验层必关。

---

## 1. rebase 白拿清单（性能相关，origin/main 41 笔内）

| 项 | 提交 | 落点 | 说明 |
|---|---|---|---|
| 图形翻译线程化（共享层部分） | `b94670c` | `dm/gfx/nvn/hle_nvn.c`（+252）、`nvn_obj.h`（+9） | 录制/翻译/提交拆分、`snap` 不可变副本、`x_fence/x_drain` 等（§4） |
| 翻译线程补充 | `44a6770` | `hle_nvn.c`（+52） | WaitEvent/WaitSync 退 nop（单队列按序成立）；`dm_nvn_sub_cpu_ns()`/`sub_cyc()` 测量钩子；呈现间隔日志改 p5/p50/p95；`present_cfg` 支持 `hz_auto` |
| 显存池按需驻留（Metal 层） | `e2a0c9a` | `nvn_mtl.m` | 约少占 0.5 GB；**Android 等价物需在 `nvn_vk.c` 池路径实现**（§6.2） |
| 3D 比例自动降档控制器（平台无关） | `afad65c` | `dm/host/hle_oe.c`（`scale3d_ctl_step` 新规则）、`dm_custom.c`（q3d 设置项） | 共享 C；本树旧版将在 rebase 后被替换（§6.1） |
| 每帧翻译量 −15% / 渲染修复（v1.0.16） | `afad65c` 等 | `nvn_mtl.m` | 具体行为以 rebase 后 diff 为准 |
| 存档合并修复（防丢装备） | `9de307e`（v1.0.17） | `dm_cloudmerge.c`（新）、`dm_cloudsync.c` | 与性能无关，但必须同步（正确性） |
| `nvn_pace.h` 增量 | `44a6770` | `dm/gfx/nvn/nvn_pace.h`（+11） | **基础版已在树**（`hle_nvn.c:26` include、:1189-1190 使用）；增量新增 `nvn_pace_hz`（刷新率整除档）等（§6.3） |

---

## 2. 分块带宽（现状 → 机会点 → 哨兵）

### 2.1 现状（实测）

- **render pass / framebuffer 缓存复用**：`rp_get()`（nvn_vk.c:4676，`g_rp_cache[256]`，键含 `clearmask`）、`fb_get()`（:5997，`g_fb_cache[512]`），**从不销毁**（历史上销毁导致 Adreno addr=0 崩溃，:5973-5982 注释）。
- **附件 loadOp/storeOp 全量清单（6 处，无遗漏）**：

| 位置 | 附件 | loadOp | storeOp | stencil |
|---|---|---|---|---|
| :1474-1477 | 交换链 | CLEAR | STORE | DONT_CARE |
| :4686-4689 | 游戏通道颜色 | CLEAR/LOAD（`clearmask`） | **STORE** | DONT_CARE |
| :4698-4701 | 游戏通道深度/模板 | CLEAR/LOAD（0x100） | **STORE** | CLEAR/LOAD（0x200）+ **stencilStoreOp=STORE 固定** |

- **清屏**：唯一途径是 `pass_begin` 的 `loadOp=CLEAR`（清除值 :6282-6304）；另有 `clear_tex()`（:7726，`vkCmdClearColorImage` 传输路径，pass 外）。**无** `vkCmdClearAttachments`、**无** clear-only pass。
- **通道划分**：每次 `OP_SET_RT`（:7854-7870）都 `pass_end()` 重开；`subpassCount=1` 恒一（:1499、:4731），**无 subpass 合并**、无"同目标连续通道"追踪。
- **可判定信号**：`g_st.clear_c/clear_d/clear_s`（客人显式 clear，:7875/:7882）→ `clearmask`（:6216-6226）；`present_blit` 回退 pass 的 `initialLayout=UNDEFINED`（:1601-1608）。**无**"首绘几何全覆盖"检测。

### 2.2 机会点（按风险从低到高）

| # | 机会点 | 证据 | 安全启用条件 | 建议开关 |
|---|--------|------|------------|---------|
| 1 | 交换链 `loadOp=CLEAR→DONT_CARE`（`UNDEFINED` 布局 + 首绘 blit 全覆盖） | :1474、:1595-1608 | 保留 blit 失败回退分支的 CLEAR | `DM_VK_SWAP_DONTCARE_LOAD` |
| 2 | `stencilStoreOp=STORE→DONT_CARE`（模板无后续读取） | :4701 单点 | 确认帧内无模板采样/回读 | `DM_VK_STENCIL_DONTCARE_STORE` |
| 3 | 颜色附件 `storeOp=STORE→DONT_CARE`（帧末无人读） | :4687 | 需"pass 结束前不再被采样/回读"追踪（对齐 read-back 池 :6048） | `DM_VK_COLOR_DONTCARE_STORE` |
| 4 | 客人已下 clear 的颜色附件 `loadOp=LOAD→CLEAR/DONT_CARE` | :4686、:6216-6223 | 客人 `clear_c[i]` 已置位即为全覆盖信号（保守可用） | `DM_VK_ATT_CLEAR_ON_PRIM` |
| 5 | `clear_tex` 并入后续通道 `loadOp=CLEAR`（省一次传输清除） | :7726-7762 | 清除后紧跟该图作为附件写入、无中间读取；注意布局差异（`g_lay_att`/`g_genall`） | `DM_VK_MERGE_CLEAR_TEX` |
| 6 | 同目标相邻通道合并（消除重复 Begin/End 与 LOAD） | :7854、:4731 | 需新增"通道间无人读"分析；先从同目标相邻 pass 做起 | `DM_VK_MERGE_SAME_TARGET` |

**先做 #1 + #2**（单点改动、风险最低），再按哨兵结果推进 #3/#4。

### 2.3 哨兵核查协议（照抄上游教训）

1. 用 `DM_VK_FORCECLEAR`（:1070）把目标清成特殊值（品红）→ 执行目标路径 → 回读（复用 `shot_take_attachment` 回读池 :6048）→ 扫品红残留。
2. **先用反例证明探针能报红**（人为构造未全覆盖首绘），探针的"0 红"才可信；回读与目标必须同队列/同帧（上游踩过"读到上一帧一直假绿"）。
3. `DM_VK_DRAWLOG`（:954）配合定位通道列表。

### 2.4 风险

- "首绘全覆盖"目前只能靠客人 clear 信号保守判定；无几何检测 → #1 的 blit 路径与 #4 的 clear 信号是当前可依赖的两条。
- `clearmask` 入 renderpass 缓存键（:4666）：改 loadOp 会产生更多 `VkRenderPass` 对象（`RP_MAX=256`，溢出回退 :4679）——开关切换后留意缓存量。
- 回读/截图路径（`g_shot_serial`、:6048-6051）依赖 read-back 池：凡被回读扫描的附件 `storeOp` 不可 `DONT_CARE`。
- 深度被采样（`INPUT_ATTACHMENT` :4469/:5644 保护的用途）时 `loadOp` 不可 `DONT_CARE`。

---

## 3. 管线缓存（VkPipelineCache）

### 3.1 现状（实测）

- **三处创建全部传 `VK_NULL_HANDLE`**：`pso_get()` → `vkCreateGraphicsPipelines` :6985；`pso_cs_get()` → `vkCreateComputePipelines` :7514；测试笔 :6612。全库无 `vkCreatePipelineCache`。
- 软缓存：`g_pso2[4096]`（:6440）**线性扫描**（`pso_key()` :6448，键含 program serial / renderpass 句柄 / 全固定功能状态，不含 SPIR-V 内容）；**无淘汰**，满 4096 后新管线不再缓存；`g_pso_created` 计数、前 200 条逐条 dump（:7006-7023）。
- 计算管线：64 槽直接映射（`serial % 64`，:7499-7505），冲突顶掉（历史上每次重建实测 6.7ms/次→3fps）。
- 着色器：构建期已译好 SPIR-V（`DM_ROOT/shader/vk_refl.tsv` + `vk_data.bin`，:4166-4180；`DM_VK_DIR` 可覆盖）；运行时按需 `pread` + `vkCreateShaderModule` 缓存（`shader_mod()` :4300-4329）。**无运行时翻译**。
- 预热：**Android 无**。iOS 有 `dm/gfx/nvn/pso_manifest.tsv`（397 行：181 S / 13 C / 其余 R）+ `warm_start()` 后台串行队列（nvn_mtl.m:2520、:2537），env `DM_PSO_MANIFEST` 可关——**清单文件与 Android 同目录约定，可直接当种子**。

### 3.2 落地方案

1. **建 cache 对象**：设备初始化处（`vk_boot` 一带）`vkCreatePipelineCache`；启动时尝试读 `$DM_TEMP/pipeline_cache.bin`（dm_env.c:96 已设 DM_TEMP）作 `pInitialData`，读失败回退空创建；退出/挂起（`dm_gfx_suspend`）时 `vkGetPipelineCacheData` 落盘。
2. **三处创建传 cache**（替换 `VK_NULL_HANDLE`）；缓存文件加**版本魔数 + 设备标识**校验，不匹配即弃（驱动升级失效是常态）。
3. **预热**：新增后台**串行**线程读 `pso_manifest.tsv` 预建（对齐 iOS `dm.nvn.pso-warm` 串行队列语义）；R 行键格式（iOS 金字塔字符串）需映射到 Vulkan `pso_key()`（或改按 (program, rp, state) 三元组重建）——**映射层是主要工作量**。
4. **不做穷举**：只预热清单几百条；其余按需创建 + `g_pso2` 留存。
5. **开关**：`DM_VK_PIPELINE_CACHE=1/0`（默认开），对齐 iOS `DM_PSO_MANIFEST=off` 惯例。
6. **并发注意**：Vulkan 规范允许同一 cache 并发创建（驱动内串列化），但建议预热线程串行 + 与回放线程错峰；将来上翻译线程时，把"新管线创建"也归到翻译线程（管线创建天然属于翻译侧）。

---

## 4. 翻译线程化（上游模型 → 我们的适配）

### 4.1 上游新模型（`b94670c`，共享层 `hle_nvn.c` + `nvn_obj.h`）

- **游戏线程只入队**：`nvnQueueSubmitCommands` → `x_push(XJ_SUBMIT, …)` 立即返回；句柄数组拷贝随任务带走。
- **翻译线程**（pthread "nvn-translate"，USER_INTERACTIVE QoS，8MB 栈）：`x_run` 顺序执行 `XJ_SUBMIT`（`mtl_sub_begin → 逐句柄 mtl_exec/mtl_exec_rest → mtl_sub_commit`）、`XJ_PRESENT`（`mtl_present` + 票号 `g_xtk`）、`XJ_FRAME`（`mtl_set_frame` + 实时性声明）。
- **同步原语**：`x_fence()/x_wait(job)/x_drain()`；背压队列 `XQ=32`；`DM_NVN_SYNC=1` 退为同步原地执行（A/B 对照基准）。
- **不可变副本**：`tex_t.snap` / `prog_t.snap` + `snap_tex/snap_prog/snap_drop`——命令载荷存副本而非游戏对象指针（否则在途命令被游戏改写污染）。
- **同步点重接线**：`nvnQueueFenceSync→x_fence()`；`nvnQueueFinish`/`dm_gfx_suspend`/`read_texels` 前 `x_drain()`；出图改票号（`x_present_ticket` → `mtl_present_wait`）。
- **Metal 侧 26 行适配（Vulkan 对等参照）**：资源计数原子化；跨线程提交前打包画法开关（`mtl_host_latch/mtl_host_apply`）；`mtl_read_texels` 非提交线程先等上传冲刷。

### 4.2 我们的现状与适配面（已核实）

- `nvn_vk.c` 实现 `mtl_*` 符号组（`mtl_init/mtl_pool_add/mtl_addr/mtl_sub_begin/mtl_sub_commit/mtl_last_fence/mtl_wait/mtl_finish/mtl_drain/mtl_exec/replay_exec/mtl_present/…`）；**回放链是真实实现**（`replay_exec` 解释记录流，`OP_DRAW→emit_draw→pass_begin`、`OP_SET_RT→pass_end` 全在）。
- **当前同步翻译**：`mtl_exec` 在游戏线程（提交路径）直接翻译；提交 `tl_submit`→`vkQueueSubmit` + 时间线信号量；命令缓冲 `g_rec_cb[4]` 轮转复用前 `wait_timeline`。
- **三类改动**：
  - **a) rebase 白拿**：`snap` 字段与命令载荷副本、`x_*` 队列、同步点重接线（全在共享层）。
  - **b) 与 Metal 同构的通用适配**（对应 26 行）：`mtl_host_latch/apply`（冻结位/强制全画打包过线程）；资源计数原子化；`mtl_read_texels` 的上传冲刷等待。
  - **c) Vulkan 专有**：命令缓冲槽/时间线（`g_rec_cb`、`g_tl`、`g_tl_val`）与描述符集/管线复用目前**无锁**——翻译线程落地时需外部同步或每槽独立；单队列提交（WaitSync nop 前提）保持；出图票号 `(ticket-1)%VK_FRAMES` 与翻译线程排队后的映射需重核（对应 Metal `x_present_ticket`）。
- **前提**：等 rebase 带来 `nvn_obj.h` 的 `snap`/`mtl_host_latch`（本树头文件还没有），否则新 `hle_nvn.c` 编译不过。

---

## 5. 测量协议（现有 → 缺口 → 最小落地）

### 5.1 现有工具（实测）

| 设施 | 位置 | 说明 |
|---|---|---|
| 驱动调用慢点告警 | nvn_vk.c:72-73 `VCALL` | 单调用 >30ms 打 `[vk] 驱动慢` |
| 每帧 7 分项计时 | :75-83 | 合计 >100ms 打 `[vk] 帧分项` |
| 周期统计 | `prof_frame_report()` :1645-1659 | 每 300 帧打印累计计数（回放/绘制/通道/blit/上传） |
| 出图分项 | :1677-1679 | 近 N 帧的等重放/等栅栏/取图/blit/提交/上屏 |
| **显示间隔日志** | `hle_nvn.c:1189-1196` | `@display_ms` = 相邻显示间隔（"掉帧看这个"）+ 实际呈现间隔（滞后 8 次，−1=被丢弃）——**可直接作为 >25ms 判据的数据源** |
| 出图节拍 | `DM_PRESENT_HZ`（hle_nvn.c:1069，缺省 60；0=不限帧仅剖析）、`mtl_fps_target()`（nvn_vk.c:2424，`DM_FPS=30\|60`） | 帧率钉制 |
| 场景判据 | `dm_hud_info`（dm_hud.h:16，`area/x/y/valid/dead`），diag.c:1062-1064 每拍发布 | 游戏内存判场景，Android 可直接复用 |
| 延迟打点 | `DM_LAT`（dm_cursor.h:96-99）+ front_android.c 日志转发 | 手指→游戏生效；**缺出图时间戳闭环** |

### 5.2 缺口（对上游口径）

| 上游口径 | 缺口 |
|---|---|
| ① 同进程切换 A/B + 配对差 + 符号检验 | **无运行时热翻转**（env 进程启动只读一次）；无配对统计脚本 |
| ② 热机稳态尾 30s | 无稳态窗口聚合 |
| ③ 掉帧按上屏间隔 >25ms | **无掉帧计数器**（`@display_ms` 数据在，但没有"计数+结论"） |
| ④ 游戏内存判场景 | 已有（dm_hud），直接用 |
| ⑤ 校验层必关 | 现状 OK（release 不带 VkLayer；`DM_VK_VAL=1` 仅诊断轮） |

**注**：上游新增的 `dm_prof`（afad65c）**Android 不能直接用**——它走 Apple `thread_selfcounts`（dm_prof.c:10-11）。Android 等价物 = `clock_gettime`/CPU cycle 计数桩（可选，后置）。

### 5.3 最小落地（建议实现形态）

1. **热翻转开关**：新增 `dm/host/dm_toggle.h/.c`（纯 C）：`dm_toggle_get(name)` 每帧重读 env（或每 10 帧），支持 `DM_VK_*` 热翻转；默认值与现状一致。
2. **掉帧计数**：在出图收尾处（present 附近）用 `@display_ms` 同源数据计 `>25ms` 次数，随 `[vk] 统计` 行每 300 帧输出。
3. **A/B 配对脚本**：`dm/android/tools/ab_pair.sh`：同进程按固定帧数交替翻转开关 → 每对记录帧时/掉帧 → 配对差 + 符号检验 p 值（Python `scipy.stats.wilcoxon` 或手写符号检验）→ TSV 归档。
4. **稳态窗口**：启动 ≥5 分钟且 `dm_hud` 在局（`valid && !dead`）后，只聚合**最后 30s**。
5. **延迟闭环**：present 处补一条 `DM_LAT("present …")`，与前端的 `DM_LAT("click"/"ui_touch …")` 相减 = 手指→上屏延迟。

---

## 6. 其他现成收益

### 6.1 3D 比例自动降档（v1.0.16）

- 上游：控制器在**平台无关** `dm/host/hle_oe.c`（`scale3d_ctl_step`，5 档 0.70→0.50、floor 0.60；掉帧窗 10s/60s 两级、回升 30s、打回后 90s 滞回；`DM_3D_SCALE_DYN=0` 可关）；设置项在 `dm_custom.c`（q3d 行）；`diag.c` 喂上屏间隔。
- 本树：**旧版**简化控制器（2 档 0.72/0.70）已在；q3d 设置项没有。
- 动作：rebase 白拿新控制器 + 设置项；Vulkan 侧把渲染比例接进渲染目标/放大路径（对应 Metal 的 `render_scale` 常量，nvn_mtl.m:3004-3005 偏移 192）——**接线是实施项，规则不用自己写**。

### 6.2 显存池按需驻留

- 上游改的是 **Metal 层**（`nvn_mtl.m`：`res_add/res_commit` 延迟到首次取址；iOS 驻留集机制，未用池不 pin，少占 ~0.5GB）。
- Android 等价物：在 `nvn_vk.c` 的池路径（`mtl_pool_add`/`mtl_addr` 一带）做**按需绑定**（延迟 `vkBindBufferMemory`/驻留到首次被 GPU 使用），细节实施时定。**不阻塞当前优化**，归入后续。

### 6.3 nvn_pace 与呈现节拍

- `nvn_pace.h` **基础版已在树**（`hle_nvn.c:26/1189-1190` 已用：present 只入队、显示时刻按节拍器排）。rebase 增量（`44a6770` +11）新增 `nvn_pace_hz`（刷新率整除档：144→72、165→82.5、240→60，含 NTSC 59.94/119.88 余量）+ `present_cfg hz_auto`。
- Vulkan 侧动作：把 `nvn_pace`/`nvn_pace_hz` 接到 Vulkan present 路径（对齐 `DM_PRESENT_HZ` 的整除档，避免 144Hz 屏上 60 帧的 2/3 拍抖动）；与上游 60fps 实践一致（出图跟显示刷新、逻辑 25 拍不变）。

---

## 7. 优先级与顺序建议

**可独立先行（不依赖 rebase，建议马上排）**：
1. §5 测量基建（热翻转开关 + 掉帧计数 + 配对脚本）——**先有尺子再谈优化**；
2. §3 VkPipelineCache（纯 `nvn_vk.c` 内部改动 + 清单映射）；
3. §2 机会点 #1/#2（单点 DONT_CARE）+ 哨兵协议搭好后推进 #3/#4。

**依赖 rebase（与 UI 工作线的 F0 是同一个 rebase）**：
4. §4 翻译线程（共享层白拿到手后做 b/c 类适配）；
5. §6.1 3D 降档接线；§6.3 pace 增量接线。

**验收口径**：全部走 §5 协议（同进程配对 + 热机稳态 + `@display_ms` 判据 + `dm_hud` 场景）；跨局比对不作收益归因。

---

## 附录 A · 证据索引（本快照行号）

| 主题 | 位置 |
|---|---|
| 附件 loadOp/storeOp | nvn_vk.c:1474-1477、4686-4701 |
| rp/fb 缓存 | nvn_vk.c:4663-4679（rp_get/`g_rp_cache`）、5973-6019（fb_get） |
| 通道划分 | nvn_vk.c:7854-7870（OP_SET_RT）、6114-6399（pass_begin）、6403（pass_end） |
| 清屏 | nvn_vk.c:6216-6226（clearmask）、7726-7762（clear_tex）、969/971/1070（开关） |
| 回读/截图 | nvn_vk.c:6048-6051、8074 |
| 管线创建 | nvn_vk.c:6985、7514、6612；软缓存 6440-6475；计数 7006-7023 |
| 着色器 | nvn_vk.c:4160-4329；`vk_refl.tsv`/`vk_data.bin` 4166-4180 |
| 预热清单 | dm/gfx/nvn/pso_manifest.tsv（397 行）；nvn_mtl.m:2520/2537 |
| 翻译线程（上游） | `git show b94670c -- dm/gfx/nvn/{hle_nvn.c,nvn_obj.h,nvn_mtl.m}`；`44a6770` |
| 回放实现 | nvn_vk.c:7796（replay_exec）、8007-8008（OP_DRAW）、8046（收尾） |
| 测量 | nvn_vk.c:72-83/1645-1679；hle_nvn.c:1189-1196（`@display_ms`）；dm_hud.h:16；dm_cursor.h:96-99 |
| 3D 降档 | 本树 hle_oe.c 旧版；origin/main `afad65c`（hle_oe.c/dm_custom.c）；nvn_mtl.m:3004-3005 |
| pace | dm/gfx/nvn/nvn_pace.h；hle_nvn.c:26/1189；origin/main `44a6770` |
| dm_prof（Apple 专有） | origin/main `afad65c`（dm/host/dm_prof.c:10-11 `thread_selfcounts`） |

## 附录 B · 开放问题 / 实施前必核对

1. **树的对齐**：你在跑实验的树与本快照的差异（尤其 `nvn_vk.c`）——所有行号实施前复核。
2. `pso_manifest.tsv` R 行 → Vulkan `pso_key()` 的映射层方案。
3. 哨兵探针实现细节（复用 `shot_take_attachment` 的可行性；同队列/同帧要求）。
4. `VkPipelineCache` 落盘时机（退出 / 挂起 / 周期）+ 版本魔数与设备标识的校验字段。
5. 翻译线程落地时命令缓冲/时间线的同步方案（每槽独立 vs 外部锁）与出图票号重映射。
6. §6.2 按需绑定的具体机制（延迟 `vkBindBufferMemory` vs 池分段惰性分配）。
7. `dm_toggle` 的粒度（每帧 vs 每 10 帧读 env）与对帧时不稳定的影响（配对设计里固定切换点）。
