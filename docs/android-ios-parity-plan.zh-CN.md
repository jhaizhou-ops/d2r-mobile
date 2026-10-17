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

## 0. 逐条点名问题的答案

| 你点的名 | iOS 是怎么做的 | 根因/落点 |
|---|---|---|
| **摇杆只能走、不能跑** | 跑步是**改写层**保证：`touch-run-v4` 规则三条补丁让 player0 无视精力门、不扣精力、精力为 0 也不降走 | 已有实现，**重出模块即生效**（P0） |
| **iOS 连 A/B 按键都没有** | ① 手柄↔触屏两套界面**布局**（padmode：把布局前缀串改写成触屏目录）+ 键位图标透明 + 25 条文案旁路（"按 A 使用"→"双击使用"）；② 面板态五个手柄圆钮**缺省不给**（`DM_TOUCH_NAV_PAD` 关，`dm_tc_nav_mask(0)=0`，front_ios.m:183-185） | 需要 overlay 数据（触屏布局来自玩家 RomFS）+ padmode 移植 + 消息状态机（P1/P4） |
| **血条不一样** | 局内底栏整个是宿主画的 v4（经验条 / Lv / 生命法力数值字 / 腰带角标 / 难度数字 / 金色十字），数字来自 `dm_hud`（读游戏内存），素材由 `gen_hud_v4.py` 生成；**首领血条**= 游戏自带的 HUDMonsterHealth 被 overlay 布局挪到屏幕顶部 + 宿主往 `TouchBossPct` 写百分比（dm_cursor.c:2307-2320） | 需要 dm_hud 数据 + 素材生成 + 自绘层重写（P3） |
| **菜单模式根本不该存在** | iOS 没有"模式"：① 局外页面 overlay 直接隐藏，触摸=原生点击（dm_ui_touch→鼠标消息，front_ios.m:177）；② 局内面板开着=导航态，被禁用的目标不占手指、触点走界面点击+手势；③ 一切由"UI 消息流 + 面板栈"自动驱动（touch_ui_message，front_ios.m:795-824） | 需要 UIMSG 状态机 + dm_cursor 鼠标层（P1/P2），做完删掉安卓的手动切换钮 |
| **"游戏设置也涉及到了"** | 是：挂存档前写 `Settings.json` 缺省 4 键（显示物品=开关式、说明条热键=关、小地图放右、进局自动开）+ 首次安装语言键 + 「大字体」兜底改回关 + 共享仓库预置 + 4 份掉落过滤规则文件（hle_fs.c:417/441/450/475/501、dm_lootfilter.c） | 整段在 `#ifndef __ANDROID__` 里——安卓一个字节都没写（P5） |
| **打开游戏后的第一屏** | ① 跳过 Logo 视频/标题页、直达主菜单，且**保持离线**（0x8019ec 切点，联网检查/断网弹窗/回标题页都绕开）；② 主菜单换成**触屏版（键鼠式）界面**：选角页右侧存档列表、**「线上 / 离线」页签挪走（只有离线）**、无「电影动画」入口、角色卡下加三个原生按钮（进入游戏 / 创建新角色 / 返回，dm_cursor 驱动）、构建号与版本提示；模式页只留标准/专家一行 + 极乐世界；联机大厅按钮/建局齿轮等联机控件一并挪走 | ①**切点两边都有**（modules/main.cuts.tsv 第 7 条，含修好的 0x8019c8 目标）；②布局在 touch_layout.py（选角/建角/模式页整套改造）→ **P4**；按钮功能靠 dm_cursor → P2；构建号提示 dm_update.c → P8d |

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

### 1.1 dm_cursor 到底管多少事（P2 的完整职责清单，别以为只是"点击"）

`dm_cursor.c` 的泵清单（dm_cursor.c:2764 那一行长行，逐项）：点击注入（鼠标消息）、命中分类、
拖放、点世界、说明框停靠、安全区/上提/名牌停靠/珍宝匣页签/小地图/地图几何/地图居中/
**首领血条百分比**、NPC 对话菜单、佣兵页、技能绑定、成叠格、仓库加宽、面板冻结清单、
面板树安全阀（ui_panel_heal）、面板状态位发布（DM:PanelState）、一键重开遮罩、
界面代号 dm_wref（跨帧控件指针安全，dm_cursor.c:96-114）。
——**一个都不能漏**：只搬"点击"不算复刻。

---

## 1.5 README 逐条对照（「为手指重新设计」+「原版之上的新玩法」）

> 左列是 README 原话要点；"落在哪"指本文档的阶段编号。

### 为手指重新设计

| README 条目 | 实现系统 | 安卓现状 | 落在哪 |
|---|---|---|---|
| 摇杆移动、斜推方向精准 | dm_touch_controls（iso_fix 缺省 1） | 已编已工作 | ✅ 已有 |
| **跑（不是走）** | 改写层 touch-run-v4 三条"忽略精力"补丁 | 无（native 产物） | **P0** |
| 点 NPC / 点目标物件自动寻路 | dm_world（w_hit：身体框/菱形/选择框/名字标签 → `main+0x4ace20` 由游戏自己走） | 无 | P2 |
| 点地面走路 | **游戏自己的触摸屏链**（宿主"空地点按=不发令"，鼠标点击落世界才走路；iOS 面板态点面板外故意不发点击，就是为了别让脚跑起来，dm_touch_controls.c:597 注释） | 原始触摸屏已接通（unclaimed→dm_touch_set） | **待实测**：可能已可用；P2 后按代码语义精确对齐 |
| 门/传送门/洞口整扇可点 | dm_world 的 sprite 选择框兜底（dm_world.c:380-390） | 无 | P2 |
| 技能按下就放/按住连放 | 手势层（共享）+ 前端 pulse 收回表 | 手势层有；前端 pulse 是简化版 | P2（照 front_ios.m:656-690 补全） |
| 拖动任意角度瞄准 | svc86 瞄准门（touch 产物）+ 手势层右轴 | 无（native 产物） | **P0**（svc 处理）+ 已有手势层 |
| 附近有怪自动锁定 | 选怪排名门 OBS 7（hle_target.c） | 门未在模块 | P0 后可选挂（P8b 精修） |
| 两组技能、副手一键放（自动换手/显示剩余次数） | 手势层 ZL 领先/换手时序（共享）+ 绑定桥（次数/Ⅱ 角标） | 手势层有；次数显示无 | P2（行为）+ P7（显示） |
| 首领血条置顶+百分比 | overlay 挪 HUDMonsterHealth 到顶 + dm_cursor 的 TouchBossPct 泵 | 无 | P3 |
| 物品单击/双击/长按/拖放 | dm_cursor 命中分类 + dm_actions（装备/卸下/买卖/取叠）+ 手势层定型的 ui_drag/shop_drag | 无 | P2 |
| 双击装备穿上/卸下 | dm_actions（dm_item_equip/unequip） | 无 | P2 |
| 拖到商人任意位置卖 | shop_drag + dm_ui_shop_buy_at | 无 | P2 |
| 成叠双击取 1 / 长按取 3 | stack_take_seq→dm_stash_take(1)/long→(3) | 无 | P2 |
| 长按背包空格一键整理 | dm_actions（dm_inv_sort 0/3/4） | 无 | P2 |
| 系统键盘输入/中文名/留空随机 | hle_swkbd + hle_charname + dm_namegen；Android 专有键盘桥已在 | 桥在；三条规则不在 | P6 |
| 全面屏铺满、避刘海圆角 | 已做（cutout→dm_tc safe insets） | ✅ 已有 | — |
| iPad 真全屏、面板居中 | dm_uiscreen + 图形层界面矩形 + 覆盖层 | 无（手机无需；平板后置） | P8e（平板） |
| 3D 画面加亮 | dm_bright | ✅ 已有 | — |
| 手柄：自动切手柄布局/面板按键提示/碰屏切回 | dm_padmode + dm_gamepad（iOS GameController 框架） | 无（Android 手柄后端也是 die 桩） | P4（布局与切换）+ P8e（Android 手柄后端） |

### 原版之上的新玩法（这一层全部依赖 P1-P4 的地基，统一放 P8 分段）

| README 条目 | 主要实现文件 | 依赖的门/切点 | 落在哪 |
|---|---|---|---|
| 秘境（100 层、随从、+技能点） | dm_rift.c / dm_rift_ui.c / dm_rift_hud.*（任务条） | 门 16/17/22、svc89（rift exit）、0x8c（save act） | P8c |
| 终局首领连战 / 极乐世界 / 刷怪积分牌 | dm_bossrush.c / dm_elysium.c / dm_kills.c | 门 15（事件消息改写）等 | P8c |
| 佣兵六个新装备位 | 0x8d/0x8e（merc slot）对应宿主 + dm_* | 已在 touch 产物切点里 | P8c |
| 编年史仓库 / 储藏箱加宽 / 珍宝匣 / 百宝囊 | dm_vault_ui.c / dm_cursor.c（wide_stash、trove 页签泵）/ dm_pouch.c | 面板状态位 + 覆盖层页签布局 | P8a |
| 增益技能自动补（含副手自动换手补） | dm_autobuff.c | — | P8b |
| 自动喝药两档 / 佣兵自动喂药 / 腰带自动捡药 / 双击腰带补满 | dm_actions.c（autopot 段、belt_fill、beltauto 相关） | — | P8b |
| 掉落过滤（噩梦起） | dm_lootfilter.c（4 份 .fltr 预置）+ 设置开关 | — | P5（文件）+P8b |
| 自动拾取（符文宝石进百宝囊） | dm_pouch.c | — | P8b |
| 高价值掉落提醒（光柱/音/震动三档） | dm_dropfilter.c（svc87）/ hle_tact.c（svc88 掉落音）/ dm_drophaptic.c | svc87/88 | P8b |
| 符文之语助手（长按带孔底材） | dm_runeword.c + 覆盖层 TouchRwBox | — | P8b |
| 小地图指引（小站/出入口/任务终点/屏幕边名字） | dm_map_goals（dm_world.c）+ 覆盖层 _info 层 | — | P8b |
| 省掉的琐事：免费鉴定 | dm_cursor 目标泵（dm_ui_identify_hovered） | — | P2 即含 |
| 省掉的琐事：免费回城 | dm_freetp.c（回城钮在触屏布局位置） | — | P8b（钮已在手势层布局） |
| 省掉的琐事：进局落小站旁 | svc8a（JOIN_SPAWN，hle_tact.c） | svc8a | P8b |
| 省掉的琐事：一键重开 | dm_cursor 重开遮罩钩子 + 布局 TouchRestart | 覆盖层按钮 | P8b |
| 省掉的琐事：商人货架可刷新 | dm_shoprefresh.c | — | P8b |
| 存档：切后台/划掉/退出立刻存 | diag.c（dm_request_save / dm_save_wait）+ guest_clock_freeze | — | P8e（安卓生命周期补全） |
| 中英切换（选项→自定义页） | dm_custom.c + 覆盖层自定义页 + settings_lang_apply | — | P5（语言键）+ P8d |
| 分享诊断日志（暂停菜单） | iOS 壳功能；Android 用 Intent 分享 diagnostics | — | P8d |

> 说明：P8 的每一小段都以"P1-P4 已完成 + 对应功能文件编入 + 覆盖层布局已带（P4 一次性生成
> 全套）"为前提；单个功能互不阻塞，可按玩家价值排序（建议顺序 a→b→c→e→d）。
>
> **明确不在本方案范围的（README 里其余部分）**：
> - 性能数字（60 帧 / 1 秒进局 / 3.8W / 续航）：iPhone 实测值；Android 有独立性能线
>   （`docs/android-perf-notes.md`：默认档 2624-075、热包线等），本方案只负责把界面机制换成
>   iOS 那套，完成后性能线照旧跑。
> - 安装/签名/游戏文件准备（`docs/GAME_FILES`、做包工具）：Android 是 APK + 外部数据目录
>   （已就绪）。
> - iOS 内测包专有的三样（iCloud 同步、App 内下载补充包、新版本提醒）公开版本来就没有 → Android 不需要。
> - 老机型贴图包（A16 及更早的 BC 贴图）：Android 侧已有 BC 解码（bcdec）——不涉及。

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
  "仅宿主"切点都走 svc 0x80-0x8e（`main.cuts.tsv:8-24/41-42`），必须先补真分发。
  逐号策略（"入口被换"=必须完整实现；"调用被换"=可以先原样代调）：
  | svc | 用途 | 形态 | P0 做法 |
  |---|---|---|---|
  | 0x80 | TACT BLTE 块 zlib 解码 | **入口被换**（原指令没了） | **必须实现**：照 `hle_tact.c` 的 `tact_decode_zlib` + libdeflate（编 5 个源） |
  | 0x86 | 技能瞄准门（拖技能定方向） | 调用被换（保底 original 0x2b4ef8） | 整段移植 `svc_private_skill_aim` + 内嵌 `main.aim.tsv` 参数 |
  | 0x85 | 技能渲染门（不画原版技能格图标） | 分支门（w8 决定走 4460/4440） | 移植；padmode 未接前 `dm_padmode_current()` 桩=手柄态 → w8=0 照画（安全） |
  | 0x81/0x82 | C2S 投递（测试回放） | 调用被换（orig 0x632510 / 0x6310b0） | 原样代调 + 审计日志（回放开关缺省关） |
  | 0x83/0x84 | Granny 逐块解码 | 调用被换（orig 0x11f4af0） | 原样代调 + 回填 w0 |
  | 0x87/0x88 | 掉落光柱 / 掉落提示音 | 调用被换（orig 0xa14dc4 / 0x470c20） | 原样代调（功能变换等 P8b） |
  | 0x89/0x8a/0x8b/0x8c | 秘境出口 / 进局出生点 / 尸体落点 / 存档幕 | 调用被换 | 原样代调（0x8a 的"落小站旁"等 P8b） |
  | 0x8d/0x8e | 佣兵装备位 | 调用被换 | 原样代调（P8c） |
  日志要求：每个代调打一行审计（只打前 N 次），方便上机看游戏在走哪条路。
- **验收**：① 跑步出现（精力槽不再扣、耗尽不降走）；② 拖技能钮能任意角度瞄准；
  ③ 日志里 svc 0x8x 无 die、画面与现模块一致（对拍截图）；④ 旧模块保留备份
  （现有 `modules.bak-r40-20261016` 同款做法）。
- **部署步骤**：重出模块 → 替换 `dodola-data/modules/` → 重打 APK（module-manifest 的
  尺寸/SHA 会变，游戏首启自动换新模块）→ 装机验收。

### P1 · UI 消息总线 + 状态机（对应"删除菜单模式"的自动态）
- **动作**：新文件 `dm/android/ui_state_android.c`：① 移植 `diag.c` 的 UIMSG 部分
  （`gate_obs_set(OBS_UIMSG, …)` + 解析 `{类型哈希, 动作哈希, 面板名}` + `uimsg_subscribe/emit`）；
  ② 移植 `dm_cursor.c` 的面板栈（`hud_stack_update` 8 个动作、常驻层 21 名单）→ `dm_hud_stack_msg`；
  ③ 直调共享的 `dm_tc_navigation`（scene）与 `dm_tc_panel_mode`；④ 面板状态位 pump 精简版
  （DM_PS_BANK/VENDOR/…，对照 `dm_cursor.c:2598-2633`）。
  > 收敛说明：这里先做**最小独立版**；P2 把真 `dm_cursor.c` 编进来后，面板栈/状态位/`dm_ui_on_uimsg`
  > 用它的原版实现替换掉最小版（两套别长期并存）。
- **接线**：局外（scene.game=假）→ 覆盖层隐藏、触摸全部直通游戏；导航态 → 目标禁用规则 +
  `dm_tc_set_joy_block`（讲话框/对话菜单）+ 死亡态；skills 快照 epoch（world/scene/rebuild）。
- **Java**：删掉"菜单操控/战斗操控"与方向/确认/返回/开始按钮；`TouchControlsView` 增加
  `setFrontend/setNavigation`；改动全在 dm/android/ 新文件与既有 Java（新建文件优先，隔离规矩）。
- **验收**：主菜单/选角页无覆盖层、进局自动出现；开背包自动进面板态（摇杆不画、目标禁用）；
  底栏（游戏自带那份）在面板态让开的行为对齐；日志里 UIMSG 消息序列与 iOS 同构。

### P2 · dm_cursor 鼠标/面板操作层（核心大件）
- **动作**：把 `dm_cursor.c / dm_cursor_item.c / dm_actions.c / dm_world.c / dm_hud.c /
  dm_uiscreen.c` 编进 Android；**钩子点**：Android 的"游戏取手柄处" =
  `input_backend.c` 的 `dm_android_input_read_npad`——接 `dm_ui_frontend_pump（替按键/发消息）/
  dm_ui_mouse_pump（每出图帧一条）/ dm_cursor_apply（改写本采样键与摇杆）`（对照
  `hle_hid.c:428-477`）；出图帧泵挂 `dm_android_front_presented`（app_jni.c:97）。
- **前端接线（照 front_ios.m 全表，别只接一半）**：
  - 触点：`dm_ui_touch(0/1/2/3)`、`dm_ui_item_query/result`、`dm_ui_drop_query`、
    `dm_ui_drag_begin/move/end/lift`、`dm_ui_world_tap`、`dm_ui_use_item/long_press/item_tap/
    tab_go/identify_hovered`、`dm_ui_bind_*`、`dm_ui_key_pulse*`；
  - 每帧：`dm_tc_tick` → `dm_ui_hand_idle` → 说明框停靠（front_ios.m:456-466）→ 补推 →
    每帧 `dm_cursor_set_screen/clear`（front_ios.m:416-476 全清单）；
  - 脉冲收回表：照 front_ios.m:656-690（120Hz 重排、"游戏已取"判据、`dm_cursor_pending`、
    `dm_tc_pulse_resume`）——现在 `touch_controls_android.c:48-62` 是简化版，要对齐；
  - 结束手势的完整分流：照 front_ios.m:570-655（含 shop_drag、pointer_dragged、
    挂起抬起 0.25s、序号差分、双击/长按/鉴定/使用/页签）。
- **已有事实**（前期核对）：鼠标注入=直接调游戏自己的键鼠消息处理入口（`root=main+0x2f05458`，
  虚表 +0x28）；命中分类=运行期按名字/虚表找控件 + 控件自己的命中函数（无地址表）；
  面板栈安全阀 `ui_panel_heal`、界面代号 `dm_wref`（dm_cursor.c:96-114）随文件带上；
  空地点按语义=宿主"不发令"（`dm_world.c` w_tap_step 无目标即返回），点地面走路走的是
  游戏自己的触摸屏链（实测项，见 §4）。
- **验收**（对照 README"为手指重新设计"）：主菜单点"进入游戏"、选角双击、面板单击/双击/长按/
  拖放、页签、点 NPC/门寻路、说明框停靠、商店买卖拖放、成叠取 1/取 3、背包空格一键整理。
- **风险**：编译依赖（CommonCrypto/CoreGraphics 等在 Android 的替换）、跨线程纪律
  （前端只写原子量，碰游戏内存只在游戏主线程）。

### P3 · 底栏 v4 + 血条 + 首领血条（用户点名）
- **动作**：① `dm_hud` 数据：把 `diag.c` 的 `hud_tick` 读数段精简移植（单位/属性/经验/腰带/
  难度/死亡，地址同 94012）；② 素材：跑 `gen_assets.py + gen_hud_v4.py`（需要一次 RomFS 提取，
  hactool 本机已有）→ `base_full.png / ui.json / 字形 / lvl 十字 / diff_N / sil 图集 / lbl_*`；
  ③ 自绘层：`TouchControlsView` 重写为完整 overlay（照 `DmTouchOverlay.m:105-278`）：
  底栏 v4（经验条含 20 等分刻度/Lv·%/生命法力字形文本（键取 `dmHudLife/dmHudMana`，见 P5
  i18n）/腰带角标/难度数字与 1–8 选择条/金色十字的两态与"有点可加"红光）+ 技能钮图标
  （剪影优先、充能用完置灰）+ 摇杆 + 调换帧钟（只在变化时重绘）。
- **首领血条**：overlay 布局把 `HUDMonsterHealth` 挪到顶部 + `TouchBossPct`（P2 的 pump 里带上）。
- **验收**：同场景与 iOS 截图对拍（数字、位置、颜色）；掉血/升级/喝药/难度切换跟手。

### P4 · overlay 数据管线 + padmode（A/B 提示消失、双击使用、前端页换触屏版）
- **动作**：
  ① RomFS 一次性提取（hactool：NSP → RomFS 文件树）；
  ② 跑 `touch_layout.py`+`gen_romfs_overlay.py`+`touch_strings.py` 出全套 overlay（触屏布局三目录、
  透明键帽、字符串旁路表、v4 素材、`ctrl_touch` 标记）。**前端页的改造清单**（= "打开游戏后
  第一屏"的差异，全部出自 touch_layout.py 的 FRONTEND/PCFRONT 段）：
  - **选角页（主菜单）换成"键鼠版界面"**（`_pc_mainmenu`/`_pc_charselect`）：右侧存档列表整块
    内收进安全区；**「线上 / 离线」页签挪走（只有离线）**；列表文字回到主机档字号；
  - **角色卡下加三个原生按钮**（进入游戏 / 创建新角色 / 返回 一类；消息沿用游戏已有的；
    「返回」只在有存档时由宿主显示——dm_cursor 摆位与点击；"点已高亮的行再点一次"的确认
    消息改为空操作）；
  - **主菜单按钮栏去掉「电影动画」**（包里不播过场）：`pad_ribbon`/`_pc_ribbon`；
    另挪走主菜单 3 个控件（键位提示等）与通用弹窗的关键位提示；
  - **建角页**：标题下「离线」字样与底条（GameType/GameTypeBackground）挪走；名字页
    EnterNameLabel 调整；
  - **模式页只留 标准 / 专家 一行**（LadderFlag/LadderToggle/NonLadderToggle/LadderCenter
    挪走）+ 触屏版「极乐世界」勾选行（原版勾选控件 + 一行说明）；
  - 构建号与新版本提示控件（主菜单层；文字与摆位由 dm_update.c 提供——无服务端时打构建号）；
  - 过场列表、选角页底部半透明底条（LegendBackground）等一并挪走。
  ③ **磁盘优先补丁**：`romfs_image.c` 的 `romfs_image_open` 加"真文件存在则让位"
  （Android 专属文件，零上游改动；hle_fs.c:719 的调用不用动）——overlay 文件落到
  `d2r-mobile/romfs-view/...` 即生效；目录/状态查询路径同补；
  ④ 移植 `dm_padmode.c`（SHA-1 用迷你实现替换 CommonCrypto）+ 解锁 `hle_oe.c:71-73` 的
  Android `die`（按 94012 验证）+ 切换事务/输入冻结/重建计数 + 前端"碰屏=触屏模式"（手柄后置）；
  ⑤ 宿主侧文件（`ctrl_touch` 标记、`touch_strings.tsv`、v4 的 `ui.json`）走
  `DM_ROMFS_OVERLAY` 读取——Android 侧要在 `app_jni.c` 里把它设为
  `"<romfs-view>:<宿主文件目录>"`（冒号多层，hle_fs.c:266-281 已支持；避免把宿主小文件
  混进游戏 rom 根）。
- **验收**：① 启动后直入选角页（无 Logo/标题页、无断网弹窗）；② 选角页是触屏版（键鼠式、
  右侧列表、**无「线上/离线」页签**）、三个按钮可点（P2 后）；③ 模式页只有 标准/专家 +
  极乐世界；④ 无「电影动画」入口；⑤ 底栏与技能格上的 a/b/x/y 消失；⑥ "按 A 使用"变
  "双击使用"（25 条逐一抽查）；⑦ 设置页不适用行隐藏；⑧ 两套布局随触碰/手柄自动切。

### P5 · 游戏设置预置与语言
- **动作**：`settings_seed / low_vision_off / settings_lang_apply / stash_seed` 的 Android 解锁
  （逻辑是通用文件操作；`dm_sys_lang()` 给 Android 实现——读系统 locale）；`dm_lootfilter.c` 的
  4 个 `.fltr`；**"Always Run" 键**纳入缺省表（P0 已从补丁层保证跑步，这算双保险+可关）；
  **编入 i18n 层**：`dm_i18n.c` + `dm/i18n/ui_text.tsv`（v4 的生命/法力字形标签键
  `dmHudLife/dmHudMana`、键盘框文案、自定义页标题都取自它；P3 依赖）。
- **验收**：删档重进 → 选项页各项符合触屏缺省；语言跟随系统；共享仓库有预置。

### P6 · 文本输入收尾
- **动作**：`hle_charname.c`（中文名放行）、`dm_textinput.c`（数字框=数字键盘+夹上限，
  判据是控件虚表 `main+0x20b91c0`）、`dm_namegen.c`（留空随机名）；面板标志
  `g_swkbd_name_panel` 的维护（现在只有 diag.c 里那份，Android 无——P1 的 UIMSG 里补）。
- **验收**：中文名、2–15 字节、`-`/`_` 至多一个、留空随机名、金币数量框。

### P7 · 技能图标与绑定桥
- **动作**：`dm_touch_game_bridge.c/state/identity` 编入（门 6 已在 P0 的 touch 产物里）；
  `dm_touch_native_art` 读图标（sil 图集来自 P4）；技能钮画图标 + 右上「Ⅱ」+ 右下次数角标 +
  空位十字（照 DmTouchOverlay.m:196-235）；`svc85` 渲染门接上（P0 的桩换成真判：触屏模式才藏
  原版图标）。
- **验收**：换武器组/换技能后图标跟着变；副手技能显示"按一下先换武器"与次数与 iOS 一致。

### P8 · 原版之上的玩法层（地基完成后按价值分批）
> 前置：P1–P4 完成；每段以"对应功能文件编入 dm/android/ + 需要的门/切点已挂"为条件，
> 互不阻塞。建议顺序 a→b→c→e→d（对照 README「原版之上的新玩法」逐条）。
- **P8a 仓库三件套**：`dm_vault_ui.c`（编年史仓库）、`dm_pouch.c`（百宝囊）、dm_actions 的
  仓库/取叠段、dm_cursor 的 wide_stash 与 trove 页签泵、个人仓库页码（dm_pages/PgPage 文本）；
  overlay 的 wide_stash/trove/vault 布局（P4 一套生成里已含）；面板状态位 DM_PS_VAULT 接 P2。
- **P8b 省心刷宝**：`dm_autobuff` 自动补增益；自动喝药两档 + 佣兵喂药 + 腰带自动捡药 + 双击补满
  （dm_actions；beltauto）；`dm_lootfilter`（4 份 .fltr，P5 已预置文件）；`dm_dropfilter`
  （svc87 光柱变换）；svc88 掉落音变换；`dm_drophaptic` 三档震动（Android 输出侧 Vibrator，
  判定共享）；`dm_runeword` 符文之语助手；`dm_map_goals` + 覆盖层 _info 名牌层（小地图指引）；
  `dm_freetp` 免费回城（触屏布局钮已在）；svc8a 进局落小站旁；`dm_shoprefresh` 货架刷新；
  一键重开（重开遮罩 + TouchRestart 钮）。顺手项：怪物血条名字后标抗性（diag.c 既有实现）。
- **P8c 秘境与终局**：`dm_rift / dm_rift_ui / dm_rift_hud`；`dm_bossrush` 首领连战；
  `dm_elysium` 极乐世界；`dm_kills` 积分牌；佣兵六装备位（svc8d/0x8e 从"代调"换真变换）；
  门 16/17/22、svc89/0x8c 从"代调"换真变换。
- **P8d 自定义页与分享**：`dm_custom`（开关收集、语言开关落地）+ overlay 自定义页
  （"选项 → 自定义"）；`dm_update.c`（主菜单的构建号/新版本提示文字与摆位；无服务端时只打
  构建号）；分享诊断日志（Android 用 Intent 分享 diagnostics 打包）。
- **P8e 手柄 / 平板 / 生命周期收尾**：Android 手柄后端（Java KeyEvent/onGenericMotion → npad；
  现在 `dm_pad_set` 是 die 桩）+ padmode 手柄侧 + 底部按键提示条（legend_pump）+ 碰屏切回触屏；
  平板真全屏与面板居中（dm_uiscreen_set_panel + 图形层界面矩形）；存档生命周期
  （dm_request_save/dm_save_wait 移植 + guest_clock_freeze 前后台序列，照 front_ios.m:1163-1179；
  Android 现在只有基础 pause/resume）。

---

## 3. 顺序与依赖（一张图）

```
P0 模块 touch 产物 ──→ 跑步立刻修；门 6-22 就位；svc_host_call 真分发
P1 UIMSG 状态机 ──→ 局外/面板态自动 ──→ P2 鼠标层（局外直点/面板手势）──→ P8 玩法层（a→b→c→e→d）
P4 overlay 管线 ──→ 触屏布局/文案/素材/v4 底图/前端页换触屏版 ──→ P4 padmode ──→ A/B 消失、选角页变触屏、双套切换
P3 底栏 v4 + 血条 + 首领血条（素材出自 P4 生成链，可先手工提取）
P5 设置 / P6 文本 / P7 图标（互相独立，可并行穿插；P7 依赖 P0 的门 6 与 P4 的图集）
```

> 每阶段收尾跑 `dm/android/tools/sync_surface_audit.sh`（动上游文件数 35 只许降）。
> 新代码优先新建文件进 `dm/android/`；动上游只在既有 Android 分支上加小钩子。

## 4. 待上机核实清单（按阶段）
1. **P0 后**：跑步（精力槽不再掉、不降走）；拖技能钮任意角度瞄准；svc 0x8x 日志无 die；
   同场景截图对拍与旧模块一致。
2. **P0 后**：摇杆满偏轴值（`ANDROID_NPAD_RETURN` axes，预期 ±32767）。
3. **P1 后**：UI 消息流日志与 iOS `[touch-receipt]` 同构；主菜单/选角无覆盖层；开背包进面板态。
4. **P2 后**：点 NPC/门/物件寻路；**点地面走路**（README 说自动寻路，代码里宿主"不发令"——
   实测游戏自己触摸屏链的行为，以实测为准）；面板单击/双击/长按/拖放逐页跑一遍（背包/箱子/
   商人/方块/技能树）；说明框停靠；成叠取 1/长按取 3；长按空格整理。
5. **P3 后**：底栏 v4 数值/位置/颜色与 iOS 截图对拍；首领血条置顶 + 百分比。
6. **P4 后**：启动直入选角页（无 Logo/视频/标题页、无断网弹窗）；选角页=触屏版（无「线上/离线」页签、右侧列表内收、三个按钮可点）；模式页只有标准/专家+极乐世界；无「电影动画」；A/B 提示图标消失；"按 A 使用"→"双击使用"（25 条抽查）；设置页不适用的行隐藏；
   磁盘优先补丁（romfs-view 放一个假文件，游戏读到盘上那份）。
7. **P5 后**：删档重进 → Settings.json 四键 + 语言键落盘正确；共享仓库存档存在。
8. **P6 后**：中文名、2–15 字节、`-`/`_` 至多一个、留空随机名、金币数量框数字键盘。
9. **P7 后**：技能图标随换装/换组变；副手角标与次数。
10. **P8 各段后**：掉落过滤开关、自动喝药阈值、符文之语助手、百宝囊自动拾取、一键重开、
    秘境入口与随从、积分牌、自定义页开关与语言、分享诊断。

## 5. 风险与回滚
- **P0 风险**：touch 规则带来其它行为差（瞄准/技能渲染）→ 旧模块备份，随时回滚；逐条对日志。
- **P2 风险**：dm_cursor 编译依赖与线程纪律 → 分批编入、先只做命中分类，再上拖放。
- **性能**：自绘层变重（底栏/图标）是 CPU canvas——做前后帧时间 spot 测（此前 UI 通道 GPU
  仅 0.78ms，留有余量）。
- **隔离**：模块产物不进仓库；`hle_oe.c` 解锁用 Android 分支小改；`romfs_image.c` 是 Android 专属。
