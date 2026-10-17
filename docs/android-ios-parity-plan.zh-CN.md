# Android 一比一对齐 iOS 触屏体验：方案（2026-10-17 版）

> **这份文档是什么（人话）**：把 iPhone 版"为手指重新设计"的那一整套（含底栏血条、
> 面板直接点、A/B 按键消失、摇杆跑步、游戏设置预置）搬到 Android 的分阶段实施方案。
> 依据 = 三个深读子线程的报告（front_ios.m / dm_cursor.c / 设置·padmode·HUD·文本·绑定桥）
> + 本机源码与改写器核对。写作约定：大白话，专有名词保留英文。
>
> **先看两个结论（本文最重要的两件事）**：
> 1. **跑步的根因找到了**：iOS 用的是改写器 `touch-run-v4` 规则域，里面有三条"跑步不看精力"
>    的补丁（`dm/tools/patch_nso.py:927-938` 一带："Run选择仅player0忽略精力门"、"仅player0
>    跳 stat10 扣除/耗尽降走"、"仅player0 跳 STA 零值自动 Stop"）。**安卓的模块是 `native`
>    产物，0 条触屏规则补丁**（核对：`dodola-data/modules/main.cuts.tsv` 里 "touch-run-v4"
>    出现 0 次）——所以安卓跑两步就没精力、自动变走路。**重出模块（换成 touch 产物）就能解。**
> 2. **设备跑的就是 94012（= iOS 同一构建，NSO 构建号 db0507f3…）**：iOS 代码里那些
>    `main+0x…` 地址表在安卓上**原样可用**；观测门 1–5（UI 消息总线等）**已经在设备模块里**。
>    缺的不是"另一个游戏版本"，而是"把已经写好的 iOS 宿主层编进 Android 构建 + 接线"。

---

## 0. 三个点名问题的答案

| 你点的名 | iOS 是怎么做的 | 根因/落点 |
|---|---|---|
| **摇杆只能走、不能跑** | 跑步是**改写层**保证：`touch-run-v4` 规则三条补丁让 player0 无视精力门、不扣精力、精力为 0 也不降走 | 已有实现，**重出模块即生效**（P0） |
| **iOS 连 A/B 按键都没有** | ① 手柄↔触屏两套界面**布局**（padmode：把布局前缀串改写成触屏目录）+ 键位图标透明 + 25 条文案旁路（"按 A 使用"→"双击使用"）；② 面板态五个手柄圆钮**缺省不给**（`DM_TOUCH_NAV_PAD` 关，`dm_tc_nav_mask(0)=0`，front_ios.m:183-185） | 需要 overlay 数据（触屏布局来自玩家 RomFS）+ padmode 移植 + 消息状态机（P1/P4） |
| **血条不一样** | 局内底栏整个是宿主画的 v4（经验条 / Lv / 生命法力数值字 / 腰带角标 / 难度数字 / 金色十字），数字来自 `dm_hud`（读游戏内存），素材由 `gen_hud_v4.py` 生成；**首领血条**= 游戏自带的 HUDMonsterHealth 被 overlay 布局挪到屏幕顶部 + 宿主往 `TouchBossPct` 写百分比（dm_cursor.c:2307-2320） | 需要 dm_hud 数据 + 素材生成 + 自绘层重写（P3） |
| **菜单模式根本不该存在** | iOS 没有"模式"：① 局外页面 overlay 直接隐藏，触摸=原生点击（dm_ui_touch→鼠标消息，front_ios.m:177）；② 局内面板开着=导航态，被禁用的目标不占手指、触点走界面点击+手势；③ 一切由"UI 消息流 + 面板栈"自动驱动（touch_ui_message，front_ios.m:795-824） | 需要 UIMSG 状态机 + dm_cursor 鼠标层（P1/P2），做完删掉安卓的手动切换钮 |
| **"游戏设置也涉及到了"** | 是：挂存档前写 `Settings.json` 缺省 4 键（显示物品=开关式、说明条热键=关、小地图放右、进局自动开）+ 首次安装语言键 + 「大字体」兜底改回关 + 共享仓库预置 + 4 份掉落过滤规则文件（hle_fs.c:417/441/450/475/501、dm_lootfilter.c） | 整段在 `#ifndef __ANDROID__` 里——安卓一个字节都没写（P5） |

---

## 1. iOS 前端全景（五层）与"已经共用的东西"

```
① 手势模型层（共享 C，安卓已编）   dm_touch_controls.c：摇杆/瞄准/双击/长按/拖放/腰带/回城
② 状态层（iOS 前端 + dm_cursor）   UIMSG 消息流 → scene/面板栈 → frontend/navigation 模式
③ 鼠标/UI 操作层（dm_cursor 全家） 触点→游戏鼠标消息；命中分类；拖放；点世界；控件观察
④ 呈现层（DmTouchOverlay + v4）   技能钮/摇杆/底栏 v4/血条/首领血条/拖货跟手图
⑤ 设置与数据层（hle_fs + padmode） Settings 缺省/语言/仓库预置/触屏布局与文案
```

**已经共用的（好消息，别重复造）**
- 同一份游戏构建 94012：iOS 的地址表（dm_cursor/dm_hud/padmode/绑定桥）直接适用。
- 观测门 **1–5 已在设备模块里**（含 UI 消息总线门 0xb1dc60、语言门、拍入口等），任何规则域都带；
  开放的 `OBS_*` 枚举在 `dm/host/dm.h:60`。**UI 消息流的原料 Android 已经有了。**
- `dm_touch_controls.c`（手势模型）已编入并通过 `touch_controls_android.c` 工作；
  `g_iso_fix` 缺省 1（斜推方向准）；`hle_swkbd.c`（系统键盘桥）已编；
  `hle_fs.c` 的 overlay 字段/`DM_ROMFS_OVERLAY` 机制在共享代码里（`mount_t.overlay`，
  hle_fs.c:146/282/312）；
- 4 条分辨率链补丁、3D 加亮、世界档 UI 修复等（性能线成果）继续有效。
- `romfs-view`：安卓把 8.9GB 镜像挂成一个**真目录树 + 镜像兜底**（romfs_image.c:229-252），
  给 overlay 提供了天然落点（缺"磁盘优先"一处，见 P4）。

**没共用的（要搬的）**
`dm_cursor.c`（2900 行，鼠标注入+面板模型+命中分类+拖放）、`dm_cursor_item.c`、`dm_actions.c`、
`dm_world.c`、`dm_hud.c`（+diag.c 的 hud_tick 读数）、`dm_padmode.c`、`dm_touch_game_bridge.c` 族、
`diag.c` 的 UIMSG 总线、`dm_textinput.c`/`hle_charname.c`/`dm_namegen.c`、
`gen_hud_v4.py`/`touch_layout.py`/`gen_romfs_overlay.py` 产物链、`DmTouchOverlay` 的自绘层。
（现 Android CMake 是显式清单：`dm/android/CMakeLists.txt:18-30`——以上都不在里面。）

---

## 2. 分阶段实施方案（每阶段独立可上机验收）

### P0 · 模块换成 touch 产物 + 宿主 svc_host_call（**已在本机实际试通**）
- **本地实测（2026-10-17）**：用本树改写器 `DM_RULESET=touch-run-v4` 重出 main/sdk/subsdk0 **成功**：
  - main：**48 条切点 / 21 个观测门**（现在设备模块：13 条 / 5 个）；sdk/subsdk0：0 切点（与现有一致）；
  - 产物已存 `/home/vick/dm-work/build/modules-touch-r44/`（r44 = `dm/PRODUCT` 当前号）；
  - **跑步补丁是纯指令切点**（`main.cuts.tsv:32-47`：RunOn/RunOff 分支与编码位改写、
    "仅player0 忽略精力门 / 跳 stat10 扣除与耗尽降走"）——**不需要宿主参与，换模块即生效**。
- **宿主侧必做（本次发现的关键风险）**：Android 的 `svc_host_call` 现在是 **die 桩**
  （`dm/android/diagnostics.c:224`：任何 0x8x 宿主调用直接 `die`）。触屏产物新增的
  "仅宿主"切点都走 svc 0x80-0x8e（`main.cuts.tsv:8-24/41-42`），必须先补真分发：
  - **0x80 TACT zlib** → 用 libdeflate 实现（vendor 在 `dm/third_party/libdeflate`，
    照 `dm/host/hle_tact.c` 编解压相关 5 个源；Android CMake 加一行）；
  - **0x85/0x86 技能渲染门 / 技能瞄准门** → 照 `hle_tact.c:274+` 移植（aim 参数表随产物
    给出：`main.aim.tsv`，ORIGINAL_CALLEE=0x2b4ef8、SVC=0x86）；
  - **其余（0x81/82/83/84/87/88/89/8a/8b…）→ "原样代调"**（每条要代的 original 目标就写在
    `main.cuts.tsv` 描述里，宿主以原参数调用它并回填返回）+ 审计日志；
    后续要哪项功能（掉落光/掉落提示音/进局落小站/秘境退出重定向）再逐个把变换接上。
- **验收**：① 跑步出现（精力槽不再扣、耗尽不降走）；② 日志里 svc 0x8x 无 die、行为与
  现在一致（对拍截图）；③ 旧模块保留备份（现有 `modules.bak-r40-20261016` 同款做法）。
- **部署步骤**：重出模块 → 替换 `dodola-data/modules/` → 重打 APK（module-manifest 的
  尺寸/SHA 会变，游戏首启自动换新模块）→ 装机验收。

### P1 · UI 消息总线 + 状态机（对应"删除菜单模式"的自动态）
- **动作**：新文件 `dm/android/ui_state_android.c`：① 移植 `diag.c` 的 UIMSG 部分
  （`gate_obs_set(OBS_UIMSG, …)` + 解析 `{类型哈希, 动作哈希, 面板名}` + `uimsg_subscribe/emit`）；
  ② 移植 `dm_cursor.c` 的面板栈（`hud_stack_update` 8 个动作、常驻层 21 名单）→ `dm_hud_stack_msg`；
  ③ 直调共享的 `dm_tc_navigation`（scene）与 `dm_tc_panel_mode`；④ 面板状态位 pump 精简版
  （DM_PS_BANK/VENDOR/…）。
- **接线**：局外（scene.game=假）→ 覆盖层隐藏、触摸全部直通游戏；导航态 → 目标禁用规则 +
  `dm_tc_set_joy_block`（讲话框/对话菜单）+ 死亡态；skills 快照 epoch（world/scene/rebuild）。
- **Java**：删掉"菜单操控/战斗操控"与方向/确认/返回/开始按钮；`TouchControlsView` 增加
  `setFrontend/setNavigation`；改动全在 dm/android/ 新文件与既有 Java（新建文件优先，隔离规矩）。
- **验收**：主菜单/选角页无覆盖层、进局自动出现；开背包自动进面板态（摇杆不画、目标禁用）；
  底栏（游戏自带那份）在面板态让开的行为对齐。

### P2 · dm_cursor 鼠标/面板操作层（核心大件）
- **动作**：把 `dm_cursor.c / dm_cursor_item.c / dm_actions.c / dm_world.c / dm_hud.c` 编进 Android；
  `hle_hid.c` 的 Android 分支接三个泵：`dm_ui_frontend_pump / dm_ui_mouse_pump / dm_cursor_apply`；
  帧泵挂 `dm_android_front_presented`（app_jni.c:97）；前端触点接 `dm_ui_touch / item_query /
  drag_* / world_tap / drop_query …`（对照 front_ios.m:556-655 的 finishTouches 全表）。
- **已有事实**（子线程核对）：鼠标注入=直接调游戏自己的键鼠消息处理入口
  （`root=main+0x2f05458`，虚表 +0x28；`LeftMouseButtonDown` 等消息哈希），**不需要新门**；
  控件观察=按名字/虚表运行期找（无地址表），命中用控件自己的命中函数探边。
- **验收**（对照 README"为手指重新设计"）：主菜单点"进入游戏"、选角双击、面板单击/双击/长按/
  拖放、页签、点地面寻路、点 NPC、说明框停靠、商店买卖拖放。
- **风险**：编译依赖（CommonCrypto/CoreGraphics 等头在 Android 的替换）、跨线程纪律
  （前端只写原子量，碰游戏内存只在游戏主线程）。

### P3 · 底栏 v4 + 血条 + 首领血条（用户点名）
- **动作**：① `dm_hud` 数据：把 `diag.c` 的 `hud_tick` 读数段精简移植（单位/属性/经验/腰带/
  难度/死亡，地址同 94012）；② 素材：跑 `gen_assets.py + gen_hud_v4.py`（需要一次 RomFS 提取，
  hactool 本机已有）→ `base_full.png / ui.json / 字形 / lvl 十字 / diff_N / sil 图集 / lbl_*`；
  ③ 自绘层：`TouchControlsView` 重写为完整 overlay（照 `DmTouchOverlay.m:105-278`）：
  底栏 v4（经验条/Lv/生命法力字/角标/难度/金色十字红点）+ 技能钮图标（先剪影）+ 摇杆。
- **首领血条**：overlay 布局把 `HUDMonsterHealth` 挪到顶部 + `TouchBossPct`（P2 的 pump 里带上）。
- **验收**：同场景与 iOS 截图对拍（数字、位置、颜色）；掉血/升级/喝药/难度切换跟手。

### P4 · overlay 数据管线 + padmode（A/B 提示消失、双击使用）
- **动作**：
  ① RomFS 一次性提取（hactool：NSP → RomFS 文件树）；
  ② 跑 `touch_layout.py`+`gen_romfs_overlay.py`+`touch_strings.py` 出全套 overlay（触屏布局三目录、
  透明键帽、字符串旁路表、v4 素材、`ctrl_touch` 标记）；
  ③ **磁盘优先补丁**：`romfs_image.c` 的 `romfs_image_open` 加"真文件存在则让位"
  （Android 专属文件，零上游改动；hle_fs.c:719 的调用不用动）——overlay 文件落到
  `d2r-mobile/romfs-view/...` 即生效；目录/状态查询路径同补；
  ④ 移植 `dm_padmode.c`（SHA-1 用迷你实现替换 CommonCrypto）+ 解锁 `hle_oe.c:71-73` 的
  Android `die`（按 94012 验证）+ 切换事务/输入冻结/重建计数 + 前端"碰屏=触屏模式"（手柄后置）；
  ⑤ 宿主侧文件（`ctrl_touch` 标记、`touch_strings.tsv`、v4 的 `ui.json`）走
  `DM_ROMFS_OVERLAY` 读取——Android 侧要在 `app_jni.c` 里把它设为
  `"<romfs-view>:<宿主文件目录>"`（冒号多层，hle_fs.c:266-281 已支持；避免把宿主小文件
  混进游戏 rom 根）。
- **验收**：底栏与技能格上的 a/b/x/y 消失；"按 A 使用"变"双击使用"（25 条逐一抽查）；
  设置页不适用行隐藏；两套布局随触碰/手柄自动切。

### P5 · 游戏设置预置与语言
- **动作**：`settings_seed / low_vision_off / settings_lang_apply / stash_seed` 的 Android 解锁
  （逻辑是通用文件操作；`dm_sys_lang()` 给 Android 实现——读系统 locale）；`dm_lootfilter.c` 的
  4 个 `.fltr`；**"Always Run" 键**纳入缺省表（P0 已从补丁层保证跑步，这算双保险+可关）。
- **验收**：删档重进 → 选项页各项符合触屏缺省；语言跟随系统；共享仓库有预置。

### P6 · 文本输入收尾
- **动作**：`hle_charname.c`（中文名放行）、`dm_textinput.c`（数字框=数字键盘+夹上限，
  判据是控件虚表 `main+0x20b91c0`）、`dm_namegen.c`（留空随机名）；面板标志
  `g_swkbd_name_panel` 的维护（现在只有 diag.c 里那份，Android 无——P1 的 UIMSG 里补）。
- **验收**：中文名、2–15 字节、`-`/`_` 至多一个、留空随机名、金币数量框。

### P7 · 技能图标与绑定桥
- **动作**：`dm_touch_game_bridge.c/state/identity` 编入（门 6 已在 P0 的 touch 产物里）；
  `dm_touch_native_art` 读图标（sil 图集来自 P4）；技能钮画图标 + 右上「Ⅱ」+ 右下次数角标 +
  空位十字（照 DmTouchOverlay.m:196-235）。
- **验收**：换武器组/换技能后图标跟着变；副手技能显示"按一下先换武器"逻辑与 iOS 一致。

---

## 3. 顺序与依赖（一张图）

```
P0 模块 touch 产物 ──→ 跑步立刻修；门 6-22 就位
P1 UIMSG 状态机 ──→ 局外/面板态自动 ──→ P2 鼠标层（局外直点/面板手势）
P4 overlay 管线 ──→ 触屏布局/文案/素材 ──→ P4 padmode ──→ A/B 消失
P3 底栏 v4 ──→ 血条/经验/Lv/首领血条（素材依赖 P4 的生成链，可先用手工提取）
P5 设置 / P6 文本 / P7 图标（互相独立，可并行穿插）
```

> 每阶段收尾跑 `dm/android/tools/sync_surface_audit.sh`（动上游文件数 35 只许降）。
> 新代码优先新建文件进 `dm/android/`；动上游只在既有 Android 分支上加小钩子。

## 4. 待上机核实清单（P0 之后第一轮）
1. 跑步：重出模块后同场景对比（精力条不再掉/不再降走）。
2. 摇杆轴值：拖动满偏时 `ANDROID_NPAD_RETURN` 的 axes（预期 ±32767）。
3. UI 消息流：挂 UIMSG 回调后打日志，核对消息序列与 iOS 的 `[touch-receipt]` 同构。
4. 磁盘优先补丁后：`romfs-view` 放一个假文件，游戏读到的是盘上那份。
5. settings 缺省落盘后：`Settings.json` 里 4+1 键与 iOS 文档一致。

## 5. 风险与回滚
- **P0 风险**：touch 规则带来其它行为差（瞄准/技能渲染）→ 旧模块备份，随时回滚；逐条对日志。
- **P2 风险**：dm_cursor 编译依赖与线程纪律 → 分批编入、先只做命中分类，再上拖放。
- **性能**：自绘层变重（底栏/图标）是 CPU canvas——做前后帧时间 spot 测（此前 UI 通道 GPU
  仅 0.78ms，留有余量）。
- **隔离**：模块产物不进仓库；`hle_oe.c` 解锁用 Android 分支小改；`romfs_image.c` 是 Android 专属。
