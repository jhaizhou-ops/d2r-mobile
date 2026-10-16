# Android 复刻 iOS UI 体验：实现设计（实现文档 v1）

> **迁移注记（整合树）**：本文写作基线为合并前的 `android-ours@0149751` 快照（自研 Vulkan 层位于 `dm/gfx/vk/`）。
> 现仓库为 `android-integration`（dodola 的 Android/Vulkan 层已合并，后端在 `dm/gfx/nvn/nvn_vk.c`），
> 文中 `dm/gfx/vk/...` 路径、iOS 参照行号与部分 `file:line` 需按新树重新对照；实现契约与验收钩子不受影响。
> Migration note (integration tree): written against the pre-merge snapshot `android-ours@0149751`;
> this tree is `android-integration` with dodola's layer merged (`dm/gfx/nvn/nvn_vk.c`) — re-check paths
> and line numbers, the implementation contract is unaffected.

> **状态：实现前置设计稿 —— 未动任何代码。**
> 总纲（结论 / 差距矩阵 / 阶段划分 / 隔离协议 / 决策点）见 [ANDROID_UI_PARITY.zh-CN.md](ANDROID_UI_PARITY.zh-CN.md)。
> 本文给出**可直接照做的实现细节**：接口快照、文件级改动、JNI 面、伪码、iOS 参照行号、验收钩子、陷阱清单。
>
> - 实现目标契约 = **`origin/main` 版本**（rebase 后；当前分支的触摸模型是旧版）。
> - 所有"照抄"处给出源文件行号，实施时逐段对照；标注为 `【核对】` 的点在实施时现场确认（附录 C 汇总）。
> - 基线：`android-ours` @ `0149751`，merge-base `9f0c257`，`origin/main` +41 提交（2026-10-16 实测）。
>
> **单位约定（全文档）**：Android 的"点" = **dp（像素 ÷ density）**。`dm_tc_*` 的宽高 / 坐标 / insets 一律用 dp；`dm_touch_set` 与 `dm_ui_*` 用归一化坐标 [0,1)。理由与换算见 §6.1。

---

## 0. 阅读地图

**数据流（端到端）**

```
┌─ Java 主线程 ────────────────────────────┐   ┌─ dmhost 线程（客体）─────────────┐
│ onTouchEvent → pointerId→槽表(0..15)     │   │ guest() 运行                     │
│   └─ JNI nativeTouch(phase,slot,x,y) ───▶│   │ 每次取手柄 hle_hid fill:         │
│ Choreographer 每帧 → nativeFrame() ─────▶│   │  ├ 读 g_f[16]（hle_touch.c）      │
│ Handler 8ms → nativePulseTick() ────────▶│   │  └ 读 dm_input 原子（hle_hid.c）  │
│ OverlayView.invalidate ← nativeSnapshot()│   │                                  │
└──────────────────────────────────────────┘   └──────────────────────────────────┘
        │  (所有 JNI 都进 front_android.c)                 ▲
        ▼                                                  │ 共享状态（任意线程可调）
┌─ front_android.c ────────────────────────────────────────┴──────────────────┐
│ g_slot[16] 槽表 + g_tc（dm_tc_state，触摸模型全部状态机）                      │
│ publish(why):  dm_touch_set → dm_cursor_set_screen/clear → dm_input_set      │
│                → belt_request/diff_request → portal 簿记                      │
│ frame_tick():  tooltip 停靠 → dm_tc_tick → hand_idle → 变更补推 → 每帧光标    │
│ pulse_tick():  "游戏已取"判据 → dm_tc_pulse_end / dm_tc_pulse_resume          │
│ snapshot():    dm_tc_state + dm_touch_game_snapshot → JSON（绘制层用）         │
└───────────────────────────────────────────────────────────────────────────┘
overlay（构建期）：gen_assets.py → gen_romfs_overlay.py → DM_ROMFS_OVERLAY（dm_env.c）
```

**三张地图**
- 线程：触摸采集 / tick / 脉冲 / 绘制全在 Java 主线程；发布 API 允许任意线程（front.h:34-37）；客体线程只读共享状态。
- 文件：§2.1。
- 契约：§1（照抄清单），全部来自 `origin/main`。

---

## 1. 目标契约速查（照抄清单）

### 1.1 front.h（dm/host/front.h，41 提交零变化，2026-10-16 复核）

| 接口 | 语义要点（实现时必须满足） |
|---|---|
| `front_run(guest)` | 有窗口：客体独立线程、主线程归 UI（front.h:4-6）。Android P2 形态：Java 起 `dmhost` 线程跑客体，主线程归 Android UI（现状 DmHostActivity.java:216-231 已经是这个形态） |
| `front_metal_layer()` | 返回绘制目标（Android：`ANativeWindow*`）；无窗口 NULL |
| `front_game_res(docked)` | 输出分辨率（宽高打包 u64）；Android 已有雏形（front_android.c:199-221），含竖屏转置与 16:9 放宽 |
| `front_text_input(...)` | **主线程弹框、调用线程阻塞**到确定/取消；无窗口返回 -1（front.h:16-24）。Android 实现见 F7 |
| `front_save_failed(err)` | 游戏线程调；前端切主线程、不阻塞（front.h:41-47） |
| `dm_touch_set(const dm_touch*, int n)` | **触点集合一有变化就整组推**；n=0 全抬起；id=槽号 0–15（按下取最小空槽）；xy=**出图画面**归一化 [0,1)；**任意线程可调**；短按锁存由宿主做（front.h:32-39） |
| `dm_host_preload_start` | 宿主装载提前起跑（与本方案无关，保持现状） |

`dm_touch = { uint32_t id; float x, y; }`（front.h:38）。

### 1.2 dm_touch_controls.h（origin/main 版本）—— 触摸模型

**初始化与布局序列（顺序不能变）**
1. `dm_tc_set_safe_insets(left_dp, right_dp)`（在 init/resize 之前设，缺省 0）
2. `dm_tc_set_edge_bottom(bottom_dp)`（系统底边条；0 = 无）
3. `dm_tc_set_tall_scale(1)`（iOS 建视图时开；Mac 关）
4. `dm_tc_layout_make(&layout, width_dp, height_dp)`
5. `dm_tc_init(&state, width_dp, height_dp)`；尺寸变化用 `dm_tc_resize` + 清空触点后推 n=0

**事件喂入（全部有 `_at` 版本，带单调秒 now_s）**
`dm_tc_begin_at / dm_tc_move_at / dm_tc_end_at`；系统取消（Android ACTION_CANCEL / 手势导航抢走手指）用 **`dm_tc_cancel_finger`（不当松手）**，整批取消用 `dm_tc_cancel_all`。
每帧 `dm_tc_tick(&state, now_s)`。

**模式与开关**
- `dm_tc_navigation(&scene, cat, act, arg)`：由 UI 消息驱动（不猜）；`scene` 由前端维护（app 起的 4 个场景回调 + `dm_hud_stack_msg` 读数）
- `dm_tc_panel_mode(nav, &scene, blocked)`：**两端共用算式**（0 游戏 / 1 面板 / 2 死亡 / 3 仅对话菜单）；blocked = `dm_hud_stack_msg`（只开对话菜单时 -1）
- `dm_tc_set_navigation_mode(state, mode)`；`dm_tc_enable_targets(state, dm_tc_nav_mask(pad))`
- `dm_tc_set_joy_block`（讲话文字框）、`dm_tc_set_portal_button / set_portal_binding / portal_ack / cancel_portal`（回城确认链）
- `dm_tc_set_holding / set_cur_stack / set_hover_unid / set_tap_box / set_panel_state / set_iso_fix`

**前端必须"看到变化就调宿主"的序号字段完整表**（dm_tc_state → 宿主 API；iOS 参照 front_ios.m）：

| dm_tc_state 字段 | 变化时前端动作 | 宿主 API | iOS 参照 |
|---|---|---|---|
| `world_tap_seq` | 世界点按 → 调 | `dm_ui_world_tap(ux,uy,wx,wy)`（u=视图归一化、w=出图归一化） | front_ios.m:629 |
| `ident_seq` | 双击未鉴定物品 → 调 | `dm_ui_identify_hovered()` | front_ios.m:619-628 |
| `use_seq` | 物品区双击 → 调 | `dm_ui_use_item()` | 同上 |
| `item_tap_seq` | 物品区单击 → 调 | `dm_ui_item_tap()` | 同上 |
| `long_press_seq` | 物品区长按 → 调 | `dm_ui_long_press()` | 同上 |
| `tab_tap_seq` | 页签点按 → 调 | `dm_ui_tab_go()` | front_ios.m:623 |
| `mouse_click_seq` | 非物品区点按 → 发一次界面鼠标点击于 `(mouse_click_x, mouse_click_y)` | `dm_ui_ptr(DM_UP_LDOWN/LUP, …)`（消费点行号【核对】） | 同上区域 |
| `belt_request` | 1=展开 / 2=收起 / `0x100\|row<<4\|col`=喝 → 调后清零 | `dm_belt_expand(on)` / `dm_belt_use(col,row)`（dm_actions.h:40-41，任意线程） | front_ios.m:222-224 |
| `diff_request` | 难度档选择 → 调后清零 | `dm_hud_request_players(n)`（dm_hud.h:34，任意线程） | front_ios.m:225 |
| `pulse_button` + `pulse_epoch` | 施放脉冲 → 按"游戏已取"判据收回 | `dm_tc_pulse_end(state, epoch)`（返回是否有变化 → 有则 publish） | front_ios.m:640-690 |
| `dm_tc_pulse_pending()` | 排着的下一个脉冲 | `dm_tc_pulse_resume(state)`（游戏看过一次松开后） | 同上 |
| `pointer_*` / `shop_drop_seq` / `stack_take_seq` / `tap_move_seq` / `portal_*` | 命中查询与拖放流程（F6 表） | `dm_ui_item_query/result`、`dm_ui_drop_query/origin_point/drop_last/drop_point/drop_bag`、`dm_ui_bind_*` | front_ios.m 对应段 |

**常量（照用，不调参）**：`DM_TC_AIM_SETTLE_S 0.08` / `DM_TC_BELT_SWIPE_PT 24` / `DM_TC_KEY_MIN_S 0.035` / `DM_TC_JOY_RESUME_S 0.35` / `DM_TC_JOY_GRAB_K 1.5` / `DM_TC_ZL_LEAD_S 0.17` / `DM_TC_DOUBLE_TAP_S 0.40` / `DM_TC_HOLD_USE_S 0.60`。

### 1.3 dm_cursor.h —— 前端调用子集（何时调）

> 注：本表按当前分支（pre-rebase）核对；`origin/main` 已再增（`afad65c`：界面代号 `dm_ui_gen/dm_ui_unstable/dm_wref`；`dm_hud_info.holding` 改为物品 id）。rebase 后以 `origin/main` 版本复核本表。

| API | 何时调（线程） |
|---|---|
| `dm_cursor_set_screen(x,y,w,h)` | 每次 publish、每帧（pointer_valid 时）；否则 `dm_cursor_clear()`（导航态）——front_ios.m:226,461-463 |
| `dm_ui_touch(phase,nx,ny)` | 前端页（主菜单/选角/建角/读取）点按/拖动；phase 0/1/2/3；纵向拖 >10pt 转滚动前先问 `dm_ui_fe_drag_ok()` |
| `dm_ui_world_tap` / `dm_ui_item_query` / `dm_ui_item_result` / `dm_ui_drop_*` | 见 §1.2 表与 F6 |
| `dm_ui_drag_begin/move/end/lift` | 界面连续手势（纵向滚动 / 横向滑块），手势层 `dm_tc_state.finger[].ui_drag` 定型后转发（任意线程） |
| `dm_ui_bind_begin(key_mask)` / `dm_ui_bind_active()` | 技能绑定流程；流程中把点按改写为 `tap_button`（光标到点+Y） |
| `dm_ui_key_pulse(mask, frames)` | 替玩家按键（如脉冲路径收起后的补按键） |
| `dm_ui_hand_idle(idle)` | **每拍**报："没有手指、没有脉冲、没有光标请求、界面触碰没在处理"=1（手上物品守门）——front_ios.m:452 |
| `dm_ui_lifecycle(background)` | 进/出后台（F7） |
| `dm_ui_map_full/is_full`、`dm_ui_minimap_rect` | 小地图交互（F6） |
| `dm_ui_safe_left/right(units)` | 安全区变化时（2160 高基准的"布局单位"） |
| `dm_ui_free_cursor()` / `dm_ui_touch_active()` / `dm_ui_bind_active()` | tooltip 停靠与脉冲判断的条件串（front_ios.m:440-467） |

### 1.4 dm_input.h

`dm_input_set(buttons, lx, ly, rx, ry)`（按钮位 = NpadButton 位序，摇杆 ±32767；按下沿锁存）。`dm_input_samples()`：游戏已取手柄累计次数（**脉冲收回判据**）。`dm_pad_set`（手柄，本期可选项）。

### 1.5 到达链线程规则（为什么可以"随意调"）
- `hle_touch.c`：`g_f[16]` 共享槽 + Start/End 属性 + 短按锁存（两次读之间按下又抬起至少报一次）；`dm_touch_set` 任意线程。
- `hle_hid.c`：`dm_input_set` 原子打包；fill 路径里 `dm_cursor_apply` 闭环、`dm_pad_hook` 注入点。
- 结论：**Java 主线程直接调，不用自己加锁、不用自己防抖**（重复实现会引入漂移）。

---

## 2. Android 侧结构设计

### 2.1 文件级改动总表（全部 Android 专属，iOS 影响面 = 零）

| 文件 | 动作 | 内容 |
|---|---|---|
| `dm/android/app/src/main/java/org/d2rm/host/DmHostActivity.java` | 修改 | onTouchEvent、Choreographer/脉冲调度、Insets 监听、生命周期回调、IME 桥方法、根布局加 OverlayView |
| `dm/android/app/src/main/java/org/d2rm/host/DmOverlayView.java` | **新增** | 覆盖绘制层（F5）：读 nativeSnapshot() 重绘 |
| `dm/android/src/jni/front_android.c` | 修改 | 槽表 + `dm_tc_state` + publish/frame_tick/pulse_tick + snapshot + 全部新 JNI |
| `dm/android/src/dm_env.c` | 修改 | `setv_default("DM_ROMFS_OVERLAY", root/romfs_overlay)` |
| `dm/android/tools/build_apk.sh` | 修改 | 新增 overlay 生成步骤（F4）；可选 assets 打包 |
| `dm/android/jni/CMakeLists.txt` | 不变（若拆新 .c 再登记） | 建议实现先集中 front_android.c，减少构建面改动 |

### 2.2 Java 层（DmHostActivity）

**触摸采集（F1）**
- `onTouchEvent(MotionEvent e)`：按 `getActionMasked()` 分派；
  - `ACTION_DOWN / ACTION_POINTER_DOWN`：为该 pointerId 分配**最小空槽**（0..15），`nativeTouch(0, slot, x, y)`；
  - `ACTION_MOVE`：遍历 `getPointerCount()`，逐指 `nativeTouch(1, slot, x, y)`（一帧一次事件批量转发）；
  - `ACTION_POINTER_UP / ACTION_UP`：该指 `nativeTouch(2, slot, x, y)` 后释放槽；
  - `ACTION_CANCEL`：`nativeTouchCancelAll()`（→ C 侧对每根手指 `dm_tc_cancel_finger`，**不当松手**）。
  - 坐标 = `getX(i)/getY(i)`（相对 SurfaceView 的像素；C 侧换算）。
  - `requestDisallowInterceptTouchEvent(true)` 不需要（无滚动父容器）；`setSystemUiVisibility` 相关见 F7。
- 槽位指派的兜底：MOVE/CANCEL 时校验 event 实际指针集，清理不存在的 pointerId（对应 iOS 的 `sweepEndedSlots` 背板，front_ios.m:470）。

**帧钟与脉冲（F2）**
- `Choreographer.FrameCallback`：每帧 `nativeFrame()`（C 侧做 tick + 每帧发布 + 脏标记）。
- 脉冲：8ms 的 `Handler.postDelayed` 循环；循环体：`if (!nativePulseTick()) 停止`（C 侧在无脉冲时返回 0，Java 随停随起）。对应 iOS 的 120Hz `pulseTick`（front_ios.m:656-690）。

**OverlayView 集成（F5）**
- 根布局顺序：`SurfaceView`（底）→ `DmOverlayView`（上，`setWillNotDraw(false)`，不拦截触摸）→ 诊断 `TextView`（调试，收尾时移出）。
- `DmOverlayView.onDraw`：调 `nativeSnapshot()`（C 侧仅在状态变化时重建；Java 用 `nativeOverlayDirty()` 判断是否 `invalidate()`）；元素绘制见 F5。

**Insets 与沉浸（F7）**
- `View.setOnApplyWindowInsetsListener`：取 `systemBars() | displayCutout()` 的 px → dp → `nativeSetInsets(l,r,t,b,gestureBottom)`（C 侧分发：`dm_tc_set_safe_insets` / `dm_tc_set_edge_bottom` / `dm_ui_safe_left/right` 折算，转换公式对照 front_ios.m:150-157）。
- 沉浸：`WindowInsetsController.hide(systemBars())` + `setSystemBarsBehavior(BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE)`；`layoutInDisplayCutoutMode = SHORT_EDGES`。保屏已有（DmHostActivity.java:75 `FLAG_KEEP_SCREEN_ON`）。

**生命周期（F7）映射表**

| Android 回调 | iOS 对等事件 | 动作（front_android.c 一个 `nativeFocusEvent(ev)` 内做序列） |
|---|---|---|
| `onPause` | sceneWillResignActive（0） | 取消全部触点 + `dm_focus_set(2)` + `resign_save()`（30s 节流；iOS front_ios.m:1197-1200） |
| `onStop`（或 onPause 尾部） | sceneDidEnterBackground（1） | `dm_focus_set(3)` + `dm_ui_lifecycle(1)` + 静音 + **`background_wait_save()`（最多 2.5s 存盘）** + `guest_clock_freeze_for(1,1)` + `dm_gfx_suspend(1)`（front_ios.m:1210-1212） |
| `onResume` + `onWindowFocusChanged(true)` | sceneWillEnterForeground/becomesActive（2/3） | `dm_ui_lifecycle(0)` + `dm_gfx_suspend(0)`（当出图路径接通后）+ `guest_clock_freeze_for(1,0)` + 音频恢复 + `dm_focus_set(2→1)` |
| `onWindowFocusChanged(false)`（下拉通知栏等） | resignActive | 同 ev0 |
| 音频 / iCloud / 更新检查 | — | `dm_cloud_*`、`dm_update_foreground` 属 iOS 专有：Android 调用点跳过（【核对】符号在 Android 库中的可见性，弱符号则自动为空） |

**IME 桥（F7）**
- Java 侧普通方法（**不是** JNI native）：`public String showTextInputBlocking(String title, String sub, String guide, String initial, int maxChars)`：非主线程 → `runOnUiThread` + `CountDownLatch` 阻塞；主线程（客体线程即 Java 线程，可直接 `GetEnv` 调）→ 直接弹。
- 对话框：`AlertDialog` + `EditText`（深色主题；按钮金色可后置）；`TextWatcher` 实现与 iOS 相同的就地过滤：字符集（字母/汉字）、`-` `_` 合计 1 个、UTF-8 ≤15 字节、`max_chars`（UTF-16，`BreakIterator`/代理对整字截断）、数字框只留数字且 ≤上限；起名的"留空=随机名 / 2-15 字节 / 首尾不能是 - _ / 组字中"校验用 OK 按钮置灰 + 原因文案（规则来源 front_ios.m:1011-1063）。
- **随机名不放在 Java**：确定返回空串时由 C 侧 `front_text_input` 调 `dm_name_gen(nm, size, dm_name_taken)`（dm_actions.h:47-48，与 iOS finish 块同序），并置 `g_fe_name_no_accept`（【核对】现为 hle_swkbd.c 全局，共享代码里可直接写）。
- 客体时钟冻结：对话框期间 `guest_clock_freeze_for(2,1)`（dm.h:77 原因 2=系统键盘；iOS 由 hle_swkbd 侧处理，Android 【核对】调用位置）。

### 2.3 JNI 面（命名沿用现有静态注册风格，Java_org_d2rm_host_DmHostActivity_*）

| JNI 方法（Java → C） | 签名草案 | 说明 |
|---|---|---|
| `nativeTouch` | `void nativeTouch(int phase, int slot, float x, float y)` | phase 0/1/2/3；x,y = SurfaceView 像素 |
| `nativeTouchCancelAll` | `void nativeTouchCancelAll()` | ACTION_CANCEL |
| `nativeFrame` | `void nativeFrame()` | Choreographer 每帧 |
| `nativePulseTick` | `boolean nativePulseTick()` | 返回是否继续（false = Java 停表） |
| `nativeOverlayDirty` | `boolean nativeOverlayDirty()` | 绘制层是否需要重绘 |
| `nativeSnapshot` | `String nativeSnapshot()` | 绘制层数据（JSON，schema 见 §4.1） |
| `nativeControlMetadata` | `String nativeControlMetadata()` | 验收钩子（对齐 DmTouchOverlay.m:273-314） |
| `nativeSetInsets` | `void nativeSetInsets(float l, float r, float t, float b, float gestureBottomDp)` | px→dp 在 Java 换算 |
| `nativeFocusEvent` | `void nativeFocusEvent(int ev)` | 0/1/2/3 见映射表 |
| `nativeSetWindowGeometry` | （已有，保留） | 尺寸变化时 C 侧重置 dm_tc：`dm_tc_resize` + 清槽 + 推 n=0 |
| `nativeScriptTouch`（可选，测试） | `void nativeScriptTouch(int phase, int slot, float x, float y)` | 对齐 iOS DmScriptTouch（front_ios.m:88-97），F1 后即可加 |

C → Java（普通方法，经缓存的 `JavaVM*`）：`showTextInputBlocking(...)`（§2.2）。`JavaVM*` 在 `dm_android_main` 入口缓存（`JNI_OnLoad` 或 runHost 首次调用处，【核对】现有代码无缓存，需新增）。

### 2.4 C 侧（front_android.c）结构

```c
static struct { int used; float x, y; } g_slot[16];   // 槽表（像素坐标）
static dm_tc_state g_tc;                              // 触摸模型全状态
static int g_tc_ready;                                // 初始化完成（surface 尺寸已知）
static float g_insets[4];                             // dp
static uint64_t g_view_w, g_view_h;                   // 出图尺寸（front_game_res 的结果）
static double g_now_s;                                // 单调秒（tick/pulse 更新）

// —— 坐标（§6.1 三种坐标的边界都在这里）——
static void  game_view_rect(float *x, float *y, float *w, float *h);   // letterbox 游戏视口（像素），对照 front_ios.m:894-908
static float  px_to_dp(float px);
static void  to_view_norm(float px, float py, float *nx, float *ny);    // →[0,1)（dm_touch/dm_tc 用）
static void  to_render_norm(float px, float py, float *wx, float *wy);  // → 出图归一化（dm_ui_* 的 w 用）

// —— 发布（逐行对照 front_ios.m:216-232）——
static void publish(const char *why);                 // dm_touch_set 整组 → cursor → dm_input_set → belt/diff → portal 簿记
// —— 每帧（对照 front_ios.m:440-467）——
static void frame_tick(void);                         // tooltip 停靠 → dm_tc_tick → hand_idle → 变更补推（按键/腰带） → 每帧光标
// —— 脉冲（对照 front_ios.m:656-690）——
static int  pulse_tick(void);                         // 新脉冲记录 → "游戏已取"判据 → dm_tc_pulse_end → 续脉冲 resume
// —— 绘制快照 ——
static int  overlay_dirty; static void snapshot_build(void);
```

**关键实现注意**
- `publish` 与 `frame_tick` 的触发源：触摸事件（即时）、每帧、脉冲 tick、`dm_tc_pulse_end` 后（返回有变化才推）。
- `frame_tick` 里"tick 内键位变了就补推"（第二组 ZL 领先补发）与"belt_request 过了双击窗补推"两条路径不能漏（front_ios.m:456-458）。
- tooltip 停靠逻辑（front_ios.m:441-449）：导航态 + 无手指 + 0.3s + `dm_ui_free_cursor()` + 未持物 + 无绑定/触碰/脉冲/光标排队 → 光标停在 (2% 宽, 50% 高)；条件是共享 API 组合，照抄即可。

### 2.5 dm_env.c

```c
setv_default("DM_ROMFS_OVERLAY", "<root>/romfs_overlay");   // 外部可覆盖（现有 setv_default 语义）
```
开发流：`run.sh` 把生成物 adb push 到 `/sdcard/Android/data/<pkg>/files/romfs_overlay`；发布流（后置）：APK assets 首启解包。

---

## 3. 分阶段实现（F0–F8）

> 每阶段独立可验收、可回滚；iOS 影响面默认零（隔离协议见总纲 §3.2）。

### F0 · rebase 对齐
- **动作**：worktree 试 `git rebase origin/main`；冲突重点：`dm/ios/Sources/front_ios.m`、`dm/host/dm_cursor.c`（面板树兜底块归属）、`dm/ios/tools/*`。
- **验证**：① `dm_tc_panel_mode`、`joy_zone_r` 在当前分支 grep 命中；② `make -C dm/host` 编译过；③ Android `build_apk.sh` 过（CMake configure 需要 RomFS 在位，现状如此）。
- **回滚**：worktree 丢弃即可，主线不动。

### F1 · 触摸采集 + 坐标映射
- **改动**：DmHostActivity.onTouchEvent + 槽表；front_android.c：`nativeTouch/CancelAll`、槽存储、`game_view_rect`、`to_view_norm`。
- **算法（照抄 front_ios.m:481-558 的槽语义）**：按下取最小空槽；槽整段生命周期不变；抬起/取消清槽；`dm_touch_set` 推的是**全部活跃槽**的整组快照（不是增量）。
- **验收**：logcat 打印每帧 `dm_touch_set` 的 n 与坐标（临时日志）；多点触控（2 指摇杆+技能）槽号稳定；与 iOS 相同的手指布局下整组快照序列一致（人工比对）。

### F2 · 发布 + 帧钟 + 脉冲
- **改动**：`publish()`（对照 front_ios.m:216-232 逐行）；Java Choreographer → `nativeFrame`；8ms 脉冲循环 → `nativePulseTick`；`dm_ui_hand_idle` 上报。
- **伪码（pulse，对照 front_ios.m:656-690）**：
```
pulse_tick():
  n = dm_input_samples()
  if pulse_button:
    新 epoch → 记起点
    if !seen && n > s0: seen = now
    if seen && !free && !dm_cursor_pending(): free = now
    taken = seen && now-seen>=0.12 && n-s0>=3 && !dm_cursor_pending()
            && (hold<=0 || (free && now-free>=hold))
    if taken || now-t0 > 2.0+hold:
        if dm_tc_pulse_end(state, epoch): publish("release_cast")
  else if dm_tc_pulse_pending(state):
    if n-s0>=2 || now-t0>0.5: if dm_tc_pulse_resume(state): publish(...)
  else: return 0
  return 1   // Java 继续排 8ms
```
- **验收**：施放一次性技能（拖方向后松手）不出现"按住不放"；拖放连续两下 A 不粘连（对应 iOS 真机回归的 tcC/tcD 场景）。

### F3 · 触摸模型接线
- **改动**：`g_tc` 初始化序列（§1.2 顺序）、事件转发（`dm_tc_begin_at/move_at/end_at` + `cancel_finger`）、尺寸变化处理（`dm_tc_resize`）、导航模式来源（`dm_tc_navigation` + `dm_hud_stack_msg` + `dm_tc_panel_mode` —— `dm_hud_stack_msg` 的头文件位置【核对】）。
- **验收**：13 项行为清单第 1–4、6–9、11–12 项逐项复现（总纲 §8.1）；摇杆贴边行为 = 浮动摇杆语义（轴值按落点算）。

### F4 · overlay 接入构建
- **改动**：`build_apk.sh` 增加 `-- [0/7] overlay`：
```bash
ROMFS=${DM_ROMFS_SRC:-${DM_ROOT:-$HOME/dm-work}/data/switch/3.3.94012/Update/RomFS}
python3 dm/ios/tools/gen_assets.py "$ROMFS"                      # 与 run.sh 同链（输出目录方案见总纲 §3.2 注意项）
python3 dm/ios/tools/gen_romfs_overlay.py "$ROMFS" "$OUT/romfs_overlay"
[ -f "$OUT/romfs_overlay/data/.build.config" ] || exit 1         # run.sh:69-72 同款自检
```
- **投放**：开发流 `run.sh` push 到 `files/romfs_overlay`（dm_env.c 已给缺省路径，无需 am 参数）；发布流后置。
- **验收**：16 项可见差异逐项过（手柄提示全灭、金色 X、底栏 v4、职业竖条、文案替换……）；`DM_ROMFS_OVERLAY` 生效证据（logcat 里 `hle_fs` overlay 命中的日志）。

### F5 · 覆盖绘制层（DmOverlayView）
- **元素表（对应 DmTouchOverlay.m:156-272 的 drawRect）**：

| 元素 | 数据源（C 快照字段） | iOS 参照 |
|---|---|---|
| 5 技能钮 + 回城 + 地图 + 菜单 | `layout.target[MAIN..MAP]`（x,y,r)+按钮标签 | drawV4 前段 |
| 技能组切换Ⅰ/Ⅱ、难度格/选择条 | `layout.swap`、`layout.diff`、`layout.pick[8]`、`diff_open` | 同上 |
| 摇杆（底盘+拇指） | `layout.joy_*` + `joy_touch_*`（按着时底盘画在落点夹回处） | 同上 |
| 技能图标/绑定映射 | `dm_touch_game_snapshot(&snap)`（dm_touch_game_state.h:41）+ `dm_ta_current_set(...)`（dm_touch_native_art.h:12） | front_ios.m:298-318 推送 |
| 底栏 v4（经验条/生命/法力/Lv/腰带徽章/难度数字） | 宿主读数（dm_hud / dm_cursor 侧），【核对】iOS 的取数点（DmTouchOverlay.m drawV4） | drawV4 |
| 拖货跟手图 | `dm_ui_item_rect(seq,r)` / `dm_ui_origin_point` | front_ios.m:340-358 |
| 小地图任务终点名牌 | 挂 `_info` 层；iOS front_ios.m:374-402 | （可选后置） |
- **刷新模型**：每帧 `nativeOverlayDirty()`；只在 glyph/绑定/数值变化时 `invalidate()`（对应 iOS"只在变化时 setNeedsDisplay"，DmTouchOverlay.m:58-60 复用帧钟）。
- **素材**：`gen_assets.py` 产出的技能图集 PNG（`Sources/TouchArt/`）与底栏 v4 素材（`Gen/assets/NativeHUD/v4/`）随 APK 打包（assets 或 res/raw，实施定）；C 侧 bitmap 解码在 Java 做，View 只拿数据。
- **验收**：`nativeControlMetadata()`（§4.1）与 iOS `controlMetadata` 的 rect 数值 diff（容差 1pt）；截图并排。

### F6 · 游戏内 UI 注入闭环
- **清单**：§1.2 表的全部 seq→宿主调用 + 前端页 `dm_ui_touch`（含 `dm_ui_fe_drag_ok` 门控与纵向拖转滚动，front_ios.m:544-554）+ 连续手势 `dm_ui_drag_*` + 技能绑定流程的 `tap_button` 改写 + 命中查询链（`dm_ui_item_query→dm_tc_pointer_item`、松手前 `dm_ui_drop_query/drop_last→dm_tc_pointer_release*`、`shop_drop_seq`、`stack_take_seq`）。
- **保留**：面板树兜底（dm_cursor.c:2838-2887）不动；OBS_UIMSG 根因另立小任务（不阻塞本阶段）。
- **验收**：主菜单→建角→进游戏→背包双击/长按/拖放→商人买/卖→腰带滑动→技能绑定 全流程。

### F7 · 外壳对齐
- **改动**：生命周期映射（§2.2 表）、Insets/immersion、IME（§2.2 桥 + 规则）、`stopHost` 接 `dm_host_shutdown`（hle_lifecycle_android.c:49）、launcher 决策（总纲 §10-1）。
- **验收**：外壳场景表（后台/前台、锁屏下启动、横幅打断、起名/金币输入、沉浸全屏、保屏、退出）。

### F8 · 帧节奏收口 + 触觉
- **改动**：确认出图路径（总纲开放问题 #1）后，把 `nativeFrame` 的节拍与出图节拍对齐（目标 60，30 档可选）；`dm_drophaptic` 输出 → Java `Vibrator`/`HapticFeedbackConstants` 三档映射（判定共享，输出侧各自实现）；可选手柄（`dm_pad_set`）。
- **上游实践确认（2026-10-16 来信）**：出图节拍跟显示刷新是安全的，游戏逻辑固定 25 拍、不随出图节拍变——与本阶段目标一致。
- **验收**：`DM_LAT` 日志（dm_cursor.h:96-99）量"手指落下→游戏生效"延迟与 iOS 同档；掉落震动触发点一致。

---

## 4. 验证钩子（Android 版）

### 4.1 `nativeControlMetadata()` JSON schema（对齐 DmTouchOverlay.m:273-314）
```json
{ "view": {"w": 844.0, "h": 390.0, "scale": 1.0},
  "controls": [ {"name":"main","x":..,"y":..,"r":..,"button":0,"label":"攻击"}, ... ],
  "joystick": {"x":..,"y":..,"r":..,"touchX":..,"touchY":..},
  "skillSet": 0, "beltRows": 1, "diffOpen": false,
  "bind": {"active":0, "slot":0} }
```
用途：与 iOS `controlMetadata:`（同 schema）做**几何数值 diff**，接入既有测试工具链。

### 4.2 其他钩子
- 脚本触摸：`nativeScriptTouch`（对齐 DmScriptTouch；目标名到坐标的映射表照 front_ios.m:203-212 的 `@target` 名单：joy/swap/main/skill1-4/portal/belt1-4/menu/map/mmname/mmbody）。
- 延迟日志：用现成 `DM_LAT`（设 `DM_LAT_LOG=1`），在 JNI 入口/发布处打点。
- UI 观察：`dm_ui_watch("name@panel")`（dm_cursor.h:60-61）已有，Android 直接可用。
- 触摸回执日志：iOS `[touch-receipt]`（front_ios.m:196-201 相邻）可后置到 Android（格式对齐，供 diff）。

---

## 5. 测试矩阵（摘要）

| 阶段 | 行为验收 | 工具 |
|---|---|---|
| F1 | 槽分配/整组快照/多点 | logcat 临时日志 + nativeScriptTouch |
| F2 | publish 序列、脉冲收回、hand_idle | 双端日志 diff（相同脚本输入） |
| F3 | 13 项行为清单 | 手动 + 脚本 |
| F4 | overlay 16 项 | 截图并排 |
| F5 | 控件几何 | controlMetadata diff（±1pt）+ 截图 |
| F6 | 全流程面板操作 | 自动化脚本；保留 T8 观察 |
| F7 | 外壳场景表 | 手动（后台/锁屏/输入法/沉浸） |
| F8 | 延迟/震动 | DM_LAT 对比；震动触发点 |

---

## 6. 陷阱与注意（实施时最容易踩的）

### 6.1 三种坐标的边界（定义一次，别再混）
| 坐标 | 单位 | 用途 | 换算 |
|---|---|---|---|
| Surface 像素 | px（相对 SurfaceView） | Java 事件 → JNI | — |
| dp（"点"） | px ÷ density | `dm_tc_*` 全部（宽高/坐标/insets） | Java 读 `resources.displayMetrics.density` 一次 |
| 游戏视口归一化 | [0,1) | `dm_touch_set`、`dm_ui_touch/drag` | 先过 letterbox 游戏视口（§6.2）；clamp `.99999` |
| 出图归一化 | [0,1) | `dm_ui_world_tap` 的 w、`dm_ui_*` 多数接口 | 对照 front_ios.m 的 `world_norm`（front_ios.m:83-85） |

**为什么 dm_tc 必须用 dp**：布局几何是"点"常数（1024 高基准、`dm_tc_scale_for_size` 667×375/440 参照），喂 px 会让整套控件尺寸错位（例：2712×1220 px 屏，scale 仍被判 1，但控件按 112"点"摆 = 视觉上缩一圈）。dp 与 pt 同量级，视觉一致。

### 6.2 letterbox
游戏视口 = 16:9 居中（`front_game_res` 决定档位）；宽屏（>16:9）两侧黑边、窄屏（<16:9）上下黑边。触摸坐标必须先映射进游戏视口再归一化——iOS 的 `DmMetalView.bounds` 本身就是游戏视口，Android 的 SurfaceView 是全屏，这层差异**必须自己算**（`game_view_rect`），否则所有点击偏移黑边量。

### 6.3 不要重复实现的既有机制
- 短按锁存 / 原子采样 / 光标闭环：宿主已做（hle_touch.c、hle_hid.c、dm_cursor.c）——前端只负责"整组推 + 每帧推"。
- 防抖 / 双击判定：在 `dm_tc` 内（DOUBLE_TAP_S 等），前端不做。

### 6.4 tick 内的补推路径（容易漏）
- `dm_tc_tick` 里会改 `buttons`（第二组 ZL 领先到点补发）与产生 `belt_request`：**tick 后不能只等下一次触摸**，要按前端 iOS 的两条"tick 内补推"路径主动 publish（front_ios.m:456-458）。
- 脉冲启动后由脉冲表收尾，不在触摸路径收（front_ios.m:459-460）。

### 6.5 脉冲收回的判据
只认"游戏已取"（`dm_input_samples` + `dm_cursor_pending`），不认墙钟；2s 墙钟只是兜底（front_ios.m:652-654 注释里的真机教训：只为墙钟收 = 收早了没反应 / 收晚了粘连）。

### 6.6 ACTION_CANCEL ≠ 松手
手势导航 / 系统弹层会 CANCEL：用 `dm_tc_cancel_finger`（放掉手指按着的东西，不执行"松手"语义——否则会误触发世界点按）。

### 6.7 窗口与 Surface 的既有约定
- "客体不等窗口、窗口不等客体"（DmHostActivity.java:5-10）：触摸/绘制都要容忍"暂时没有 Surface"（无窗口时 tick 照跑，publish 照推——宿主对无窗口是安全的）。
- `surfaceDestroyed` 只放下窗口不杀游戏；OverlayView 的可见性跟随 Activity 即可。

### 6.8 隔离协议
任何对共享文件的改动（目前预期为零）走总纲 §3.2 五条 + 登记表。**特别注意**：不要为了 Android 往 `dm/ios/Sources/**` 放文件（iOS 构建目录通配会吸走）；不要在 `dm/host/` 放 Android 专属 `.c`（Android GLOB 会收、iOS 不收，制造不对称）。

---

## 7. 提交划分与回滚

| 提交 | 内容 | 回滚 |
|---|---|---|
| F0 | rebase（或 cherry-pick cfa70b3/d9e95c3 + dm_cursor hud 栈） | worktree 丢弃 |
| F1 | Java 触摸采集 + JNI + 坐标映射（临时日志） | 拿掉 onTouchEvent 即恢复现状 |
| F2 | publish + frame/pulse 调度 | 停调度线程（tick 不发） |
| F3 | dm_tc 接线 | 环境开关跳过 dm_tc（仅保留 dm_touch_set，仍可玩但无手势模型） |
| F4 | overlay 生成 + env | 不设 DM_ROMFS_OVERLAY |
| F5 | OverlayView | 不添加视图（隐藏即可） |
| F6 | UI 注入 | 开关（保留 dm_touch 基础层） |
| F7 | 外壳 | 逐项独立（IME / 沉浸 / 生命周期可分开提交） |
| F8 | 节奏/触觉 | 独立提交 |

建议：F1–F3 合并为一次"输入主链"评审提交；F4 独立（构建面）；F5–F6 一并评审；F7 逐项拆。

---

## 附录 A · 关键签名速查（实施对照用）

- `void dm_touch_set(const dm_touch *t, int n)`；`dm_touch = {u32 id; float x,y;}`（front.h:38-39）
- `void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry)`（dm_input.h:16）；`uint64_t dm_input_samples(void)`（:17）
- `void dm_cursor_set_screen(float x, float y, float w, float h)`；`dm_cursor_clear()`；`int dm_cursor_pending()`（dm_cursor.h:9-20）
- `dm_tc_*`：见 §1.2（`dm_tc_init/resize/begin_at/move_at/end_at/cancel_finger/cancel_all/tick/pulse_end/pulse_pending/pulse_resume/pointer_*/set_*/layout_make/scale_for_size/tall_scale/navigation/panel_mode`）
- `int dm_hud_stack_msg(void)`（面板开合栈；头文件位置【核对】）
- `void dm_belt_expand(int on)`；`void dm_belt_use(int col,int row)`（dm_actions.h:40-41）；`int dm_name_gen(char*,int,int(*)(const char*))`；`int dm_name_taken(const char*)`（dm_actions.h:47-48）
- `void dm_hud_request_players(int n)`（dm_hud.h:34）；`void dm_focus_set(int st)`（hle.h:56）；`void guest_clock_freeze_for(int reason,int on)`（dm.h:77）；`void dm_gfx_suspend(int on)`（dm.h:114）
- `int dm_touch_game_snapshot(dm_tg_snapshot*)`（dm_touch_game_state.h:41）；`dm_ta_glyph dm_ta_current_set(...)`（dm_touch_native_art.h:12）

## 附录 B · iOS 参照行号索引

| 主题 | front_ios.m 行号 |
|---|---|
| pushFrom（发布三件套 + belt/diff 转发） | 216-232 |
| 触摸入口/槽管理/结束清槽 | 470-558 |
| tick（tooltip 停靠、dm_tc_tick、hand_idle、补推、每帧光标） | 440-467 |
| 脉冲表（armPulseEnd / pulseTick 120Hz） | 640-690 |
| 坐标/归一化/letterbox | 216-221；83-85；894-968 |
| 技能绑定快照推送 | 298-318 |
| 拖货跟手图 | 340-358 |
| 小地图名牌 | 374-402 |
| 触觉（掉落三档） | 408-411 |
| 文本输入（规则/随机名/键盘） | 991-1106 |
| 生命周期四事件 | 1190-1216 |
| 视图创建（overlay 添加、tall_scale） | 132-160 |

## 附录 C · 实施时"现场核对"清单

1. `dm_hud_stack_msg()` 的声明位置与语义（dm_cursor.c 定义、头文件在哪）。
2. `mouse_click_seq` 的 iOS 消费点（死代码风险：确认它现在走 `dm_ui_ptr(LDOWN/LUP)` 还是 `dm_ui_send_queue`）。
3. `belt_rows/belt_open` 前端多久、从哪个宿主读数写入 `dm_tc_state`。
4. `g_fe_name_no_accept` / `g_swkbd_name_panel` / `g_swkbd_numeric_max` 的作用域与写权限（Android 共享库内是否同样可见）。
5. `dm_cloud_*` / `dm_update_foreground` 在 Android 库中的可见性（弱符号即可跳过）。
6. `dm_gfx_suspend` 在 Android（nvn_vk）中的现状（未接出图前调用是否安全）。
7. `gen_assets.py` 输出目录方案（总纲 §3.2 注意项 a/b）与底栏 v4 素材在 Android 的打包方式。
8. 出图路径现状（总纲开放问题 #1）——影响 F2 帧钟与 F8 收口。
9. `JavaVM*` 缓存位置（现有 front_android.c 无缓存，需新增）。
10. ACTION_CANCEL 在 SurfaceView 上的实际触发场景表（手势导航/多任务预览）实测补齐。
11. rebase 后核对 `nvn_vk.c` 与新版 `hle_nvn.c`/`nvn_obj.h` 的接口适配（`b94670c` 图形翻译线程化 +252 行、`44a6770` +52 行；iOS 侧同笔 `nvn_mtl.m` 有 26 行适配，Vulkan 侧需要对等物）。
12. `patch_nso.py` 的 r41 修正与 4 个新观测点在 Android 装载器产物链上的同步核对（见方案文档 §4.5）。
