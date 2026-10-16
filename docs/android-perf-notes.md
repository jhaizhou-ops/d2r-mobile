# Android 性能工程笔记（骁龙 8 Gen 1 / Adreno 730）

本文件汇总合并版 Android 端（1280×720 ~ 2168×1220 输出、1121 draws/帧营地区）逐轮实测的
性能结论。所有数字来自设备 60 帧窗口（`RENDER_PROFILE` / `RENDER_PHASE_CPU_PROFILE`）。

## 1. 帧模型（理解一切优化的基础）

营地场景典型帧 ≈ 44–53ms：

```
帧时间 ≈ CPU 翻译流水(replay ~21.5ms + begin/retire/wb/present ~6ms)
       + GPU 忙时(~35–43ms, 与 CPU 部分重叠)
       + 提交缝隙(gap ~8–14ms, GPU 空转 = 喂料不及)
```

- 现象一：`gpu_wait` 大（13–21ms）→ GPU 是墙或 CPU 喂不及（两者交替出现，看场景）。
- 现象二：帧率贴 60Hz vsync 台阶（≈50ms→20fps / 33.3ms→30fps）。**跨过台阶才有 fps 跳变**，
  所以小幅优化有时"看不到效果"直到累积跨线。
- 现象三：S8G1 上 **GPU 不带宽受限**（load/store DONT_CARE 全开无变化），
  墙是主色 pass 的着色/几何本身 + 提交节奏。

## 2. 已实施（赢）

| 项 | 效果 | 提交 |
|---|---|---|
| 写回 mprotect 批量合并（合并时丢的 dodola 版 + 主线 BUSY 复检语义） | mprotect 15480→120 次/60帧；wb 子系统 −33% CPU | `6f4235c` |
| **kind2 统一缓冲改动态描述符**（描述符内容跨 draw 稳定，偏移走绑定调用） | desc 重建 −85%（47880→7243/60帧）；计划 fallback −63%；replay 每调用 −25%；**营地 20.3→22.7fps，轻场景 29.5** | `ca12412` |
| 3D 渲染比例 94012 映射移植（iOS 同机制；含失败自动回退） | GPU 忙时 −8ms（0.8 档），代码默认关 | `e683d96` |
| `DM_GAME_RES` 出图分辨率覆盖（KRES 参数位，iOS 同机制） | UI/文字按原生出图（清晰度质变）；fps 视场景略正略负 | 本文件同期 |
| **默认关遮挡查询批量**（`DM_DISABLE_QUERY_BATCH`） | 原版把"精确遮挡查询"横跨多个 draw 长开（`VK_QUERY_CONTROL_PRECISE_BIT`），在 Adreno 上长期屏蔽 early-Z/HSR：**营地 19.8→25.2fps（+32%），gpu_wait 19.4→9.7ms** | 本文件同期 |
| **120Hz 节拍解锁**（`DM_PRESENT_HZ=120`） | 游戏请求 present interval=2；60Hz 基准下 = 33.3ms 周期 → 硬锁 30fps。**120Hz 基准 → 60fps 上限**，轻中场景实测 **37–44fps**（原锁 30），gpu_wait ~3ms → 转为纯 CPU 瓶颈 | `e46dbf4` |
| **管线缓存跨会话持久化**（`pso-cache.bin`） | 原来每次启动从零编译，游玩中首次遇到怪/特效要现场编译 340–571ms。现落盘复用：同类卡顿 3→2 窗且持续收敛 | `224c00c` |
| **BC 压缩纹理直传**（Adreno 原生支持 `textureCompressionBC`） | 原来首次绑定 CPU 解码 380–512ms（"怪瞬移"主因之一）。现块流直传：upload>100ms 窗口 9→5，同路线均帧 27.6→32.7；截图验证渲染正确 | `68852db` |
| 测试开关通道：`diagnostics/flag-*` 一个文件一个开关（access 通道，FUSE 可读） | 免重建调参 | `e683d96` |

## 3. 已实测（输 / 判负，勿再走）

| 项 | 结果 |
|---|---|
| 命令状态缓存（`command-state-cache`） | 跳过 68% 命令但 **fps 反降**（Adreno 重绑比比较便宜）→ 默认关 |
| 附件 load/store DONT_CARE | 三重组合均无增益 → 非带宽瓶颈 |
| 提交粒度 `DM_CHUNK_DRAWS`=12/24/96 | 无差/无差/−2.5fps（分块实际由 pass 边界决定，旋钮近乎无效） |
| `barrier-pass-elide` | ±0.2fps 噪声 |
| `disable-wait-elision`（关掉等待省略） | −2.3fps → 该机制有价值，保持默认开 |
| precise-sync / binding-plans 关闭 | 无益或略损（现役机制都是正收益，勿动） |
| storage-bias / temp-pool / wb-stage 关闭 | **灾难级**（−2 ~ −18fps）→ 顶梁柱机制 |
| async-drain | −2.7fps → 保持关 |
| 0.8 比例 @1600×900 旧态 | 曾负收益；**管道修好后转为正**（详见 §5） |

## 4. 关键机制速查（改代码前必读）

- **描述符**：kind2=UBO / 3,9=SSBO / 4,6=纹理 / 8=驱动表 / 10=缓冲图像。动态偏移只打 kind2；
  快照/临时路径自动回落 dyn=0；>4G 偏移回落快照（实际不会发生）。
- **绑定计划**（binding_plan_*）：只对 kind2-aligned + 采样纹理生效；动态绑定后偏移不再作废计划。
- **写回脏页**（nvn_dirty.c）：`prep_write` 批量认领相邻页 + 单次 mprotect；竞争页保留 BUSY 复检。
- **出图分辨率**：游戏读 KRES0/KRES1 线程槽（`loader.c` 装的 pthread key），值来自
  `front_game_res`（`dm/android/diagnostics.c`，现支持 `DM_GAME_RES=WxH`）；
  游戏收到 31 号通知或启动初始化时重建输出与 3D 目标（3D 内部 = 输出 × 3D 比例）。
- **3D 比例**：`android_scale3d_tick`（TryPopMsg 内调用）写渲染器 +0x66a4 并调 resize；
  94012 映射锚点已离线验证；一次性生效后靠游戏自身重建沿用。
- **探针**：`profile-detail` 开 GPU 时间戳（SEG gpu_busy）与 `RENDER_CPU_PROFILE` 分解；
  `flag-pass-timing` 开逐 pass GPU 毫秒（PASS_TIME rank 行）；开销大，只在诊断轮开。

## 5. 数据表（营地，1121 draws/帧）——全部为"预热后持续状态"公平值

| 配置 | fps | 备注 |
|---|---|---|
| 原始合并版（1600×900） | 20.3 | 基线（糊） |
| + 动态描述符 + wb 修复（1600×900） | 22.7 | |
| + 原生出图 2168×1220（3D 1.0）+ 关批量查询 | **25.1** | UI 原生锐利 |
| **+ 3D 比例 0.85（iOS 档位）** | **28.0** | 当前推荐配置 |
| Blood Moor 轻场景（3D 1.0） | 29.5* | *或含频率加成，持续估 27–30 |

CPU 分解（detail 轮，replay ≈26.8ms 含探针开销）：bind 9.1ms（最大块，描述符绑定路径）、
pool 3.3、desc 分配+更新 2.3（仅 132 次 miss）、renderpass 1.6、shader 查找 1.2、其余 ~9.3。

### 测量陷阱（重要！）

- **冷却频率加成**：设备静置后第一轮帧率可虚高 30–50%（28.9 vs 19.2 同配置）。
  一切对比必须"先烧机一轮，再 A/B"，或交替多轮取趋势。
- **场景粒子方差**：营地火盆火焰/粒子逐帧变化，同 draws 数下 GPU 负载可差 ±20%。
- `PRESENT_PARTS acquire_ms` 是**每 60 帧累计**，每帧约 0.1ms，不是每帧值。

## 6. 待办/下一步

1. 描述符绑定路径（9.1ms，最大 CPU 块）：计划校验/回退与缓存查找继续深挖。
2. 3D 比例在原生出图下的档位标定（0.85/0.8 ABAB）。
3. 动态 3D 比例控制器（iOS `dm_3d_scale_feed` 机制移植，安卓侧现为桩）
   → 流畅优先时自动降档，画质优先时锁定——"iPhone 式"体验的最后一环。
4. 极限评估：本 SoC 重场景现实区间 25–35fps（原生出图 + 3D 0.75–0.85）；
   60fps 仅在轻场景/静止视角可达，需 Apple M 级 GPU 才谈得上全程 60。
