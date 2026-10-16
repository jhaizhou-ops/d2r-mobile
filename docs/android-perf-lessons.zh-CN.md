# Android 性能工程经验教训（给未来会话 / Agent 的"别再踩"清单）

> 来源：2026-10-16 Android 移植性能工程（小米 22081212C，骁龙 8+ Gen 1 / Adreno 730）。
> 详细数字与过程见 `docs/android-perf-notes.md`（持续维护）；本文只提炼"坑 + 可操作规则"。
> 用法：动手调参或做 A/B 前先过一遍 §12 自检清单；遇到相似现象直接跳到对应条目。

## 快速速查

| # | 坑 | 一句话规则 |
|---|---|---|
| 1 | 沙箱读不到 shell 创建的文件内容 | 运行时读内容的控制文件写完必须 `chmod 664`，脚本回读校验 |
| 2 | 跨 run 对比虚高/虚低 | 强结论只用进程内交替；跨 run 只比 µs/绘制 |
| 3 | 对照没生效还当成对照 | 看 `DIAG_SWITCHES` + 每次切换必须有切换日志，无自证 = 无效实验 |
| 4 | 用小实验量小效果、用长批次量大效果 | 按"效果 ÷ 噪声"分级：大效果冒烟、小效果进程内多相位 |
| 5 | 拆掉守恒/一次性闸门 | 先问它原本在挡什么（tick 回归：37.4 vs 18.8µs/绘制） |
| 6 | 为 1-2ms 收益冒崩溃风险 | 深水优化必须带可控复现；回退要干净并写清"为什么不做" |
| 7 | 探针自扰动 | 对比必须同探针配置；绝对数字标注是否带探针（+~10µs/绘制） |
| 8 | 后台输出只留尾部 | 证据以设备侧日志为准，每 run 后 `adb pull` 存档 |
| 9 | 冷机数字当真实值 | 跑前记录热状态；热稳态与冷机可差 ~2x（30 → 17fps） |
| 10 | 启动后 touch flag 期待生效 | flag 文件是 stat 型、仅启动读；要运行中切换必须用运行时开关 |
| 11 | 同一功能 iOS 正常、Android 异常 | 先查构建期补丁的"仅宿主"剥离（两端产物 cuts.tsv 对照），再怀疑运行时 |

---

## 1. 沙箱读权限：运行时控制文件写完必须 `chmod 664`（最重要）

- **现象**：shell（adb）在 `.../files/d2r-mobile/diagnostics/` 创建的控制文件（如 `chunkd.txt`）内容正确、`cat` 可见，但 app 进程**读不到**——运行时开关永不生效，且**无任何报错**（`fopen` 失败被静默忽略）。
- **根因**：shell 创建的文件默认权限 `-rw-rw----`(660)。app 侧沙箱**允许 stat 但不总能读内容**，所以 flag 通道（`access()`）一直正常，读内容的文件间歇性失败。代码注释原文（`dm/android/app_jni.c:329-331`）："the app-side sandbox allows stat on shell-created files but not always read of their contents, so one file per switch/value"。
- **证据**：文件 660 时启动 app 25 秒，无任何切换日志；`chmod 664` 后重启，立刻出现 `[nvn-vulkan] CHUNK_DRAWS 运行中切换 → 200`（切换打印在 `dm/gfx/nvn/nvn_vk.c:3290`）。此前一次四相位 chunk 扫描因此整体无效（四相位实际都在默认值）。亦见 `docs/android-perf-notes.md:104-106`。
- **规则**：
  - **stat 型开关（`flag-*`）随便**——`access()` 不受影响；
  - **运行时要读内容的控制文件（`chunkd.txt` / `loadstore.txt` / 未来的 hz 类）写完必须 `chmod 664`**；
  - `env.txt`（K=V 环境导入，`dm/android/app_jni.c:371-377`）同样走 `fopen`，同理需要 664；
  - 脚本里写完要**回读校验**（cat 出内容 + 确认权限位）。

## 2. 跨 run 对比不可信（场景漂移 + 热差）

- **现象**：同一 build 两次运行 fps 差 40%+；曾据此得出两个**假结论**：①"翻译队列无效"（build33 vs 34，draws 124k vs 67k）；②"通道缓存掉帧"（A 尾部 124k draws vs B 66k）——两次都是拿不同场景的数字互相比。
- **根因**：走动脚本落点漂移 → 每帧绘制数/开销（场景）不同；设备热状态不同（见 §9）。
- **证据**：`docs/android-perf-notes.md:63-66`（通道缓存对照无效 + 场景错配）；`docs/android-perf-notes.md:96-98`（队列的正确裁决来自同局对照：重场景 21-29µs/绘制 vs 同步 17.5-18.4）。
- **规则**：
  - **跨 run 只比"按绘制归一化"的 µs/draw（口径 = ms ÷ draws × 60）**；
  - 对比前先确认两边 draws 量级相当（目测 ±10% 以内）；124k vs 67k、124k vs 66k 这类直接作废；
  - **强结论一律用进程内交替（运行时开关）**；跨 run 只作趋势参考，不作判负/判胜依据。

## 3. 控制组必须自证

- **现象**：通道缓存 A/B 的 B 轮 `flag-pass-cache-off` 可能**从未生效**——同机制的 `flag-resource-stats` 全程无任何输出，说明该批 flag 文件根本没被读到；对照无效，结论作废。
- **根因**：flag 无效是静默的（见 §1）；不做"生效自证"就分不清"关闭无效"与"根本没关"。
- **证据**：`docs/android-perf-notes.md:63-64`；启动打印 `DIAG_SWITCHES` 行在 `dm/gfx/nvn/nvn_vk.c:526`。
- **规则**：
  - 每次运行先看 `DIAG_SWITCHES` 行（启动时打印）确认配置实际值；
  - **每个运行时开关的切换必须有切换日志**（如 `LOADSTORE/CHUNK_DRAWS 运行中切换`、`3D 比例 A/B：…（帧 N）`）；
  - **无自证的对照视为无效实验**，不得据此下结论。

## 4. 验证深度分级：按"效果大小 ÷ 噪声"选方法

- **大效果（≥10-15% 预期，如队列预期 +40%）**：分钟级冒烟即可定生死——模式自证行 + 3-4 个窗口 + 按绘制 µs 口径。不值得上长批次（队列案例：两轮就判死；Apple 侧 +40% 的收益在 Adreno 上为零甚至负）。
- **小效果 / 噪声级（如动态比例）**：进程内交替、多相位、**跳过切换后 12-25 秒取稳态**。比例案例：同局 `1.0 ↔ 动态档` 各 60 秒 ×4 轮，结果 **17.3 vs 17.4fps 零差异**——动态比例是 GPU 余量/功耗资产，不是 fps 资产。
- **证据**：`docs/android-perf-notes.md:70-79`（§0e 动态比例 进程内交替 A/B 裁决表）；`docs/android-perf-notes.md:96-98`（队列）。
- **规则**：先估效果量级再决定投入；低于噪声的效果要么做进程内交替，要么直接承认"现口径量不出"。

## 5. 删闸门之前先看它挡住了什么（tick 回归案例）

- **现象**：动态比例改成"可反复应用"后，武装会话慢近一倍（**37.4 vs 18.8µs/绘制**，同场景）。
- **根因**：原来的一次性闸门（"与当前值一致就什么都不做"）不再生效 → 每次系统通知轮询的 `android_scale3d_tick()` 都调 `android_scale_readable()`＝**完整扫 `/proc/self/maps`（毫秒级）**，且每次扫两遍。
- **证据**：`dm/host/hle_oe.c:400-407`（readable 实现）、`dm/host/hle_oe.c:420-430`（修复注释与缓存逻辑：指针没变不重扫，~5 秒复核一次）；`docs/android-perf-notes.md:100-102`。tick 调用点：`h_oe_TryPopMsg` → `android_scale3d_tick`（`dm/host/hle_oe.c:556-562`）。
- **规则**：
  - **热路径上的昂贵校验必须缓存化**（指针变化或定时复核才重扫）；
  - **去掉守恒/一次性逻辑前，先问它原本在防止什么**。

## 6. 收益 vs 崩溃：深水优化要带可控复现，回退要干净

- **现象**：渲染对象缓存跨块持久化的尝试在**加载期可复现 abort** → 已回退。收益仅 1-2ms。
- **另一例**：跨帧通道缓存实测无收益（Adreno 的 `vkCreateRenderPass` 近乎免费；renderpass_ms 两轮无差 192/201 vs 181/198）→ 实现已回退，旧通道对象留缓存也是浪费。
- **证据**：`docs/android-perf-notes.md:31-32`（跨块持久化崩溃已回退）、`docs/android-perf-notes.md:61-66`（通道缓存试过→回退）。
- **规则**：
  - **1-2ms 的收益不值得一次崩溃**；
  - 深水优化上马前先备好可控复现路径；回退要干净（git），并把"为什么不做"写进文档，防止未来重踩。

## 7. 探针自扰动

- **现象**：`DM_PROFILE_DETAIL`（profile-detail）本身约加 **~10µs/绘制**（同场景 28.5 vs 18.8µs/绘制）。
- **证据**：`docs/android-perf-notes.md:176-177`（"开销大，只在诊断轮开"）、`docs/android-perf-notes.md:189`（replay ≈26.8ms 含探针开销）。
- **规则**：
  - **对比必须同探针配置**（两边都开或两边都关）；
  - **绝对数字要标注是否带探针**；开探针轮的数字不许和关探针轮的基线直接相减。

## 8. 后台任务输出只保留尾部

- **现象**：后台 shell 的输出 `.out` 文件只留了尾部（13 行），**丢掉中间两轮的完整结果**，证据链断裂。
- **根因**：终端/管道缓冲与截断不可控。
- **规则**：**证据以设备侧产物为准**——每次 run 结束后 `adb pull` 全部日志到 perf 目录存档；终端回显、后台 `.out` 不可作证。

## 9. 热稳态纪律

- **现象**：同场景冷机 ~30fps ↔ 热/背压下 ~17fps（整帧 59ms 中翻译侧 ~18-20ms，其余是节流后的游戏逻辑+录制）；冷却频率加成可让首轮虚高 30-50%。
- **证据**：`docs/android-perf-notes.md:87-90`（热稳态真值）、`docs/android-perf-notes.md:92-94`（"热数据翻案"——慢会话真凶是 tick 回归而非热，但热状态仍必须先记录）。
- **规则**：跑前记录热状态——`dumpsys thermalservice`（Thermal Status）、`dumpsys battery`（温度）、`/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`（CPU 频率）；
  **充电边玩 = 热源**；对比前先记录热状态，跨会话对比认准热稳态值。

## 10. 一次只动一个变量 + 运行时开关优先

- 已建成的进程内开关（运行中可切换、带自证日志）：
  - `flag-3d-quality` → 比例 A/B（钉住 1.0 ↔ 动态档；`dm/host/hle_oe.c:534-538`）；
  - `loadstore.txt` → 带宽 A/B（内容 0/1/2/3，每个 profile 窗口读一次；`dm/gfx/nvn/nvn_vk.c:3280-3283`）；
  - `chunkd.txt` → 分块提交粒度（内容 = 每多少绘制冲刷一次，0=关；`dm/gfx/nvn/nvn_vk.c:3287-3290`）。
- **flag 文件（stat 型）只在启动时读，运行中不生效**——别在启动后 `touch flag` 期待生效（曾有一次这样的空测：相位 B 未武装，见 §3）。要运行中切换就用上表的运行时开关。
- 规则：**一次只动一个变量**；能进程内交替就进程内交替（消除热差与场景漂移）。

## 11. 同一功能两端行为不同：先查构建期补丁的"作用域剥离"

现象：同样的"3D 比例 + 改分辨率函数"在 iOS（宿主）界面上完全正常，Android 上界面整体缩到 ~0.6 且上飘。
根因不在运行时代码，而在**改写期的补丁作用域**：Android 产物构建时会剥掉全部标"仅宿主"的剪口
（`dodola-build` 的 `patch_nso.py` 第 245 行：`cuts = [c for c in cuts if c[4] != '仅宿主']`），
其中 4 条正是分辨率链的前提（输出上限初值 1920×1080→4096×2160、`w & 0x740` 掩码修正 ×2、界面画布宽上限放开）。
尤其上限初值：Android 保留 1920×1080 时，启动"选分辨率"的超限分支对 2168 / 2624 输出会**跳过"窗口尺寸事件"**
（iOS 这一步会送达）→ 两端状态起点不同，之后任何目标重建都落在不同的状态上。

规则：
- 修"两端表现不一致"的 bug：先对照两端的产物 tsv（cuts / params）与改写器的 `仅宿主` 过滤，再怀疑运行时。
- 跨平台复用的"同一效果"，其补丁必须双端同语义；标了"仅宿主"但 Android 也需要 → 升为"全"。
- 模块重建的自证：与旧模块**字节级 diff 应恰好只出现预期补丁区**（本次 = 4 处、其余逐字节相同）——
  比"能跑起来"强得多的证据，10 行脚本就能做。

---

## 12. 测前自检清单（每次 A/B 前逐项打勾）

- [ ] 开关文件存在；运行时读内容的（`chunkd.txt` / `loadstore.txt`）**权限 664 且内容已回读确认**
- [ ] 启动日志含 **`DIAG_SWITCHES` 行**并核对各开关实际状态（`dm/gfx/nvn/nvn_vk.c:526`）
- [ ] **模式自证行**可见：同步/队列模式行、A/B 切换日志（`3D 比例 A/B：…（帧 N）`）、`CHUNK_DRAWS/LOADSTORE 运行中切换` 行
- [ ] 进程存活 + 无 `ANDROID_FATAL`（`dm/android/diagnostics.c:130`）
- [ ] **热状态已记录**（thermalservice / battery / cpufreq），且在非充电、可比热状态
- [ ] **归一化口径**已就位：跨 run 用 µs/draw（ms ÷ draws × 60）；同局用进程内交替
- [ ] **探针配置一致**（profile-detail 等两边相同），绝对数字标注探针状态
- [ ] 每 run 结束 **`adb pull` 全部日志到 perf 目录存档**（终端/后台输出不可作证）

---

## 附录：本仓库证据索引（file:line → 内容）

| 位置 | 内容 |
|---|---|
| `dm/android/app_jni.c:329-331` | 沙箱注释原文：stat 可、读内容不可 → 一开关一文件 |
| `dm/android/app_jni.c:332-365` | flag 表（`flag-*` → 环境变量/值） |
| `dm/android/app_jni.c:366-369` | flag 循环：`access()` 命中才 `setenv` + `ANDROID_FLAG` 日志 |
| `dm/android/app_jni.c:371-377` | `env.txt` K=V 导入（fopen 读，需 664） |
| `dm/gfx/nvn/nvn_vk.c:524-526` | `DIAG_LOADSTORE` 启动打印 + `DIAG_SWITCHES` 行 |
| `dm/gfx/nvn/nvn_vk.c:3280-3283` | `loadstore.txt` 运行时轮询 + 切换日志 |
| `dm/gfx/nvn/nvn_vk.c:3287-3290` | `chunkd.txt` 运行时轮询 + 切换日志 |
| `dm/host/hle_oe.c:400-407` | `android_scale_readable()`＝完整扫 `/proc/self/maps` |
| `dm/host/hle_oe.c:420-430` | tick 校验缓存化修复（指针变化或 ~5s 复核） |
| `dm/host/hle_oe.c:534-538` | `flag-3d-quality` A/B 轮询与切换日志（帧号） |
| `dm/host/hle_oe.c:556-562` | tick 调用点：`h_oe_TryPopMsg`（每通知轮询都进） |
| `dm/android/diagnostics.c:130` | `ANDROID_FATAL` 崩溃行格式 |
| `docs/android-perf-notes.md:23` | load/store 实测无非带宽瓶颈（哨兵核查法留作省电实验） |
| `docs/android-perf-notes.md:31-32` | renderpass 跨块持久化崩溃已回退 |
| `docs/android-perf-notes.md:63-66` | 通道缓存假结论：对照未生效 + 场景错配（124k vs 66k） |
| `docs/android-perf-notes.md:70-79` | 动态比例进程内交替裁决表（17.3 vs 17.4，零差异） |
| `docs/android-perf-notes.md:87-90` | 热稳态真值（~1500 draws ≈ 17fps） |
| `docs/android-perf-notes.md:96-102` | 队列永久关闭；tick 回归 37.4 vs 18.8µs/绘制 |
| `docs/android-perf-notes.md:104-106` | FUSE 读权限坑（660 不可读 → 664 立刻可读） |
