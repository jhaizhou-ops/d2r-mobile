# Android 与上游代码隔离分析（2026-10-17）

**任务**：把我们的代码与上游（iOS 向）代码隔离、尽量收进 `dm/android/` 类目录，降低上游同步冲突。
**结论先行**：**全隔离不可行**（移植必须接缝 loader/HLE/渲染分发）；**部分隔离可行且收益明确**，
63/98 文件已是纯新增（零冲突面），剩下 35 个共享文件按四类分级治理即可。

## 1. 现状量化（merge-base `1784563` vs HEAD）

- 全量 diff：**98 文件、+22798 / −140**。**删除仅 140 行** → 我们的工作几乎全是"新增/插入"。
- **63 个纯新增文件**（同步零冲突；前提=上游不创建同路径文件）：
  `dm/android/` 33 · `dm/gfx/nvn/` 11 · `docs/` 5 · `dm/gfx/tests/` 4 · `dm/tools/bright_lut/` 3 ·
  `dm/android/src/...` 3 · `dm/android/tools/` 2 · `dm/host/` 1 · DESIGN.md。
  **Vulkan 后端（`nvn_vk.c` + 其全部头文件）本身就在新增列**——渲染后端天然隔离是成立的。
- **35 个修改文件 = 全部是上游既有文件** = 唯二冲突面（实际是本仓库与上游的全部同步摩擦来源）。

## 2. 冲突面分级（按体量与结构）

| 级别 | 代表文件（+add/−del） | 结构性质 | 同步风险 |
|---|---|---|---|
| **A 生成物** | `nvn_thunks.S` +8392/−0 | `#ifdef __ANDROID__ [整张 Android 表] #else [上游表原样 6813 行] #endif` | **低**：上游内容在 `#else` 内逐字保留，上游改动与我们首尾插入不重叠，合并顺滑 |
| **B 大块实现** | `hle_oe.c` +341 · `hle_fs.c` +289 · `hle_thread.c` +142 · `patch_nso.py` +141 · `nvn_dirty.c` +137 · `hle_nvn.c` +116 | 函数内插入/新增块（100–350 行） | **中高**：上游改同函数即冲突 |
| **C 追加型契约** | `nvn_obj.h` +5 · `hle.h` +9 · `nvn_dirty.h` +7 · `dm.h` +3 · `nvn_mtl.h` +3 | 枚举/声明追加（≤9 行） | **中**：两侧在**同一点**追加即冲突 |
| **D 小改接缝** | 其余 ~20 个（`loader.c` +49 · `entry.S` +34 · `thread.c` +19 · `hle_*.c` 各 +4~65） | 小插入 | **低-中** |

实测佐证：2026-10-17 同步上游 5 提交时，恰恰在 `hle.c`/`loader.c`（B/C 类）撞了 2 个冲突；
而 A 类的 `nvn_thunks.S`、D 类文件 0 冲突。

## 3. 能不能隔离——分手段评估

1. **新增即隔离（已是现状，保持默认）**：新功能一律新文件/`dm/android/`；渲染后端独立。
   → 63/98 已达标，**纪律 = 后续工作默认延续此模式**。
2. **B 类外迁（最高价值）**：把大块实现抽成 `dm/android/impl/<名>.c`，共享文件只留
   **单点钩子**（3–10 行 `#ifdef __ANDROID__` 调用块）。接口=现有 HLE 函数，机械可做。
   预估：`hle_oe` 0.5d · `hle_fs` 1d · `hle_thread` 0.5d · `hle_nvn` 0.5d（其余 B 类用一次同步周期观察后再定）。
3. **C 类契约纪律**：共享头文件追加放入 **ANDROID 专用区段**（`// ==== ANDROID-BEGIN/END ====`）；
   值域型枚举（op 码等）给 Android 划**独立号段**（如 ≥0x100）——把"同点追加"变"分段追加"。
   （兼容性注意：op 码若进客体重写模块的共享编号契约，改号段必须同树重出模块——R-68 即踩此坑。）
4. **A 类规则化**：维持 `#ifdef` 双表（天然抗冲突）；追求共享文件零 diff 可把 Android 表外迁为
   独立 `.S`（构建接线在本仓库**外部**的 merged 构建树，需协调）——**评估为可选低优先**。
5. **不建议**：
   - 构建期源码补丁层（quilt）：B 类是要持续演化的真代码，patch 层会累积 rebase 地狱；只适合
     推导式小补丁。（既有二进制改写器 `patch_nso.py` 属另一维度，继续保留。）
   - 子模块/双仓拆分：需上游先引入"平台抽象接口"——属于一个上游 PR 工程，现阶段不现实。

## 4. 分阶段行动 + 同步手册

- **Phase 1（低成本，随时）**：① 本文即"同步手册"骨架 → 补 35 文件逐行策略表
  （take-theirs+重打 / keep-ours / 自动重生成）；② C 类加 ANDROID-BEGIN/END 区段标记；
  ③ 生成物与钩子约定写入 CONTRIBUTING/notes。
- **Phase 2（每文件 0.5–1d）**：B 类 top4 外迁（hle_oe → hle_fs → hle_thread → hle_nvn）。
  每步验证 = Android 构建 + 设备冒烟（FATAL=0 + 关键路径：启动/进局/场景切换）。
- **Phase 3（可选）**：`nvn_thunks.S` Android 表外迁 + `dm/android/tools/sync_upstream.sh`
  （fetch + merge + 自动输出冲突面分级报告——用本文 §2 的分级规则）。
- **预期终态**：冲突面从"35 个混合文件、8000+ 行体量"收敛为"~20 个文件 × ≤65 行 + 4 个钩子块 +
  3 个契约区段"，且每次同步有明确策略表可循。

## 附录：35 个共享文件同步策略表（自动生成，2026-10-17）

| 文件 | +/− | 级别 | 同步策略 |
|---|---|---|---|
| `dm/gfx/nvn/nvn_thunks.S` | +8392/−0 | A 生成物 | 冲突时=双表结构不重叠，正常合并；重生成以对齐 |
| `dm/host/hle_oe.c` | +341/−8 | B 大块 | keep-ours 为主；外迁计划 §4-Phase2 |
| `dm/host/hle_fs.c` | +289/−16 | B 大块 | keep-ours 为主；外迁计划 §4-Phase2 |
| `dm/tools/patch_nso.py` | +141/−30 | B 大块 | keep-ours 为主；外迁计划 §4-Phase2 |
| `dm/gfx/nvn/nvn_dirty.c` | +137/−23 | B 大块 | keep-ours 为主；外迁计划 §4-Phase2 |
| `dm/host/hle_thread.c` | +142/−9 | B 大块 | keep-ours 为主；外迁计划 §4-Phase2 |
| `dm/gfx/nvn/hle_nvn.c` | +116/−11 | B 大块 | keep-ours 为主；外迁计划 §4-Phase2 |
| `dm/gfx/nvn/nvn_entry.S` | +92/−0 | D 接缝 | 逐点小插；冲突时手工收编 |
| `dm/gfx/tests/t_dirty_slots.c` | +92/−0 | D 接缝 | 逐点小插；冲突时手工收编 |
| `dm/host/hle_account.c` | +65/−5 | D 接缝 | 同上 |
| `dm/host/hle_lifecycle.c` | +64/−0 | D 接缝 | 同上 |
| `dm/host/hle_touch.c` | +62/−4 | D 接缝 | 同上 |
| `dm/host/hle_misc.c` | +57/−6 | D 接缝 | 同上 |
| `dm/host/hle_time.c` | +56/−4 | D 接缝 | 同上 |
| `dm/host/hle_hid.c` | +56/−0 | D 接缝 | 同上 |
| `dm/gfx/hle_nv.c` | +50/−0 | D 接缝 | 同上 |
| `dm/host/loader.c` | +49/−4 | D 接缝 | 同上 |
| `dm/host/hle_net.c` | +43/−8 | D 接缝 | 同上 |
| `dm/host/hle_friends.c` | +36/−5 | D 接缝 | 同上 |
| `dm/host/entry.S` | +34/−0 | D 接缝 | 同上 |
| `dm/gfx/hle_vi.c` | +30/−0 | D 接缝 | 同上 |
| `dm/host/hle_svc.c` | +28/−0 | D 接缝 | 同上 |
| `dm/host/hle_guestthread.c` | +27/−2 | D 接缝 | 同上 |
| `dm/host/dm_bright.c` | +26/−0 | D 接缝 | 同上 |
| `dm/host/thread.c` | +19/−0 | D 接缝 | 同上 |
| `dm/host/hle.c` | +18/−2 | D 接缝 | 同上 |
| `dm/host/hle_os.c` | +15/−0 | D 接缝 | 同上 |
| `dm/host/hle_sdkinit.c` | +15/−0 | D 接缝 | 同上 |
| `dm/host/hle.h` | +9/−2 | C 契约 | take-theirs + 重贴 ANDROID 区段（先补标记） |
| `dm/gfx/nvn/nvn_dirty.h` | +7/−0 | C 契约 | 同上 |
| `dm/gfx/nvn/nvn_obj.h` | +5/−1 | C 契约 | 同上（改号段须同树重出模块） |
| `dm/host/hle_audio.c` | +4/−0 | D 接缝 | 逐点小插；冲突时手工收编 |
| `dm/host/hle_swkbd.c` | +4/−0 | D 接缝 | 同上 |
| `dm/gfx/nvn/nvn_mtl.h` | +3/−0 | C 契约 | take-theirs + 重贴 ANDROID 区段 |
| `dm/host/dm.h` | +3/−0 | C 契约 | 同上 |
