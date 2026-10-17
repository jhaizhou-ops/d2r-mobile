#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DM_TC_MAX_TOUCH = 16, DM_TC_COUNT = 12 };
enum {
    DM_TC_NONE = -1, DM_TC_JOYSTICK = -2, DM_TC_SWAP = -3, DM_TC_DIFF = -4,   /* DIFF：难度格 / 选择条占用的手指，不发按键 | DIFF: a finger captured by the difficulty cell / selector bar; sends no key */
    DM_TC_EDGE = -5,   /* 屏幕底边归系统的那一条里起手的手指：不当摇杆，松手也不当点地 | A finger that started in the strip along the bottom edge of the screen that belongs to the system: not treated as the stick, and its release is not treated as a ground tap */
    DM_TC_MAIN = 0,
    DM_TC_SKILL1, DM_TC_SKILL2, DM_TC_SKILL3, DM_TC_SKILL4,
    DM_TC_PORTAL, DM_TC_BELT1, DM_TC_BELT2, DM_TC_BELT3, DM_TC_BELT4,
    DM_TC_MENU, DM_TC_MAP
};
typedef struct { float x, y, r; uint64_t button; } dm_tc_target;
typedef struct {
    float width, height;
    int tablet, swap_above;   // 已生效的布局配置，绘制和命中判定共用 | Applied layout profile; mirrors drawing and hit testing.
    dm_tc_target target[DM_TC_COUNT];
    float joy_x, joy_y, joy_radius;   /* 没按时底盘的显示位置（静止位）与底盘半径 | where the base is shown while not pressed (rest position), and the base radius */
    float joy_zone_r, joy_zone_t, joy_min_x, joy_max_y;   /* 起摇杆区的右界与上界（左与下到屏幕边）；按下后底盘圆心的夹回界：x 不小于、y 不大于（只为底盘整圈画在屏内：屏幕边 + 半径 + 8） | right and upper bounds of the stick-start zone (left and bottom reach the screen edges); clamp bounds for the base centre once pressed: x not less than, y not greater than (only so that the whole base is drawn on screen: screen edge + radius + 8) */
    dm_tc_target swap;   /* 技能组切换键：本地开关，不直接发按键 | Skill set switch key: a local toggle; does not send a key directly */
    dm_tc_target diff;   /* 底部 UI v4 第 0 格难度：点开 / 收起 1–8 选择条，不发按键 | Bottom UI v4 cell 0 difficulty: tap to open / collapse the 1–8 selector bar; sends no key */
    dm_tc_target pick[8];   /* 选择条 8 格（第 i 格 = 人数档 i+1），在七格上方 | The selector bar's 8 cells (cell i = player-count level i+1), above the seven cells */
} dm_tc_layout;
typedef struct {
    int active, target;
    float x, y;
    float aim_origin_x, aim_origin_y;
    uint64_t aim_move_sequence;
    int aim_cancelled;
    int key_down; double key_down_s;   /* 技能钮：键是否已按下过、几时按下的（按下后先等判定时间，见 update） | Skill button: whether the key has gone down and when (after press, first wait the decision time, see update) */
    double start_time_s;
    int pointer_dragged, pointer_long;
    /* 界面连续手势（鼠标点击模式、起手不在物品区也不在面板外、位移过阈值时按起始方向定型，定型后不变）：1 = 纵向拖（滚动），2 = 横向拖（滑块）。
       手势层只定型，前端看到后把起手点与当前点转给宿主（dm_ui_drag_*），滚动 / 滑块由宿主按游戏自己的命中去做
       Continuous UI gesture (mouse click mode, started neither in the item area nor outside the panel; when displacement crosses the threshold the type is fixed by the initial direction and does not change afterwards): 1 = vertical drag (scroll), 2 = horizontal drag (slider).
              The gesture layer only fixes the type; on seeing it the front end forwards the start point and current point to the host (dm_ui_drag_*), and the scroll / slider is done by the host according to the game's own hit testing */
    int ui_drag;
    int shop_drag;   /* 从商人货架的货上起拖（不拿起）：松手按落点买 / 不买 | Drag starting from goods on the vendor shelf (not picked up): on release, buy / do not buy by the drop point */
    int locked_drag; /* 单件任务槽占用时从别的物品上起拖：不拿起，松手不发任何键 | Drag starting from another item while the single-item quest slot is occupied: do not pick up, and send no key on release */
} dm_tc_finger;
typedef struct {
    dm_tc_layout layout;
    dm_tc_finger finger[DM_TC_MAX_TOUCH];
    int joy_slot;
    float joy_origin_x, joy_origin_y;
    uint32_t enabled_targets;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    int aim_slot, navigation_mode, map_cancel_active;
    uint64_t aim_move_sequence;
    int portal_pending, portal_phase; // 0idle / 1modifier-pending / 2armed
    uint64_t gesture_epoch, portal_binding_identity, portal_scene_epoch;
    /* 松手施放的短按脉冲：按键与右轴方向同时发布，前端到时调 dm_tc_pulse_end 收回 | Short-press pulse for release-to-cast: the key and the right-axis direction are published together, and the front end calls dm_tc_pulse_end to release it when due */
    uint64_t pulse_button, pulse_epoch;
    int32_t pulse_rx, pulse_ry;
    uint64_t defer_button; double defer_at_s;   /* 第二组里点得太短的那一下：等 ZL 领先够了再补发 | The too-short tap in the second set: make it up once ZL has led long enough */
    /* 技能组：0 = 第一套；1 = 第二套（原版 Switch 按住 ZL 用 alt 槽）。是模式，不随取消/尺寸变化清零 | Skill set: 0 = first set; 1 = second set (on the original Switch game, holding ZL uses the alt slots). It is a mode and is not cleared on cancel/size change */
    int skill_set;
    /* 第二组技能（ZL 修饰）：ZL 只在手指按着主钮 / 技能钮、或技能脉冲在发时按着，且先于技能键 DM_TC_ZL_LEAD_S 按下。
       松手施放按得太短时，这一下脉冲延到 ZL 领先够了再发（dm_tc_tick 到点起）。
       Second skill set (ZL modifier): ZL is held only while a finger is on the main button / a skill button or a skill pulse is being sent, and goes down DM_TC_ZL_LEAD_S before the skill button.
              When a release-to-cast press is too short, that pulse is delayed until ZL has led long enough (dm_tc_tick starts it when due). */
    int pointer_slot;
    double last_time_s;
    int pointer_pickup_done;
    int pointer_pulse_active, pointer_pulse_pending;
    float pointer_pending_x, pointer_pending_y;
    int pointer_valid;
    float pointer_x, pointer_y;
    /* 物品手势：pointer_item = 当前指针手指落在物品区（前端在松手前按宿主命中结果置位）。物品区单击只移光标（出说明）、
       双击 = 按住 A（游戏提示：按住 = 打开 / 使用 / 装备；开着箱子时按住 Y = 移到另一侧）；非物品区单击照旧 = 移光标 + 按 A。
       pulse_hold_s > 0 = 这个脉冲要按住这么久，前端在「游戏已取走」之外再等满它才收
       Item gestures: pointer_item = the current pointer finger landed in the item area (set by the front end before release from the host's hit result). A single tap in the item area only moves the cursor (shows the tooltip),
              double tap = hold A (game hint: hold = open / use / equip; with the stash open, hold Y = move to the other side); a single tap outside the item area is as before = move the cursor + press A.
              pulse_hold_s > 0 = this pulse must be held this long, and the front end waits it out in full in addition to "the game has consumed it" before releasing */
    int pointer_item;
    /* 非物品区的点按走鼠标点击（暂停菜单按钮、布局里插回的关闭 / 修理等按钮只认鼠标）：mouse_click_mode 由前端打开；
       每次这样的点按 mouse_click_seq 加一，前端看到变化就在 (mouse_click_x, mouse_click_y) 发一次界面鼠标点击，不再按 A
       Taps outside the item area go through mouse clicks (pause menu buttons and the close / repair buttons inserted back into the layout only respond to the mouse): mouse_click_mode is turned on by the front end;
              each such tap increments mouse_click_seq, and on seeing a change the front end sends one UI mouse click at (mouse_click_x, mouse_click_y) and no longer presses A */
    /* 拖放松手处（物品抬在手指上方的那个落点）是不是物品区：前端松手前置位。不是 = 拖到面板外 = 丢弃（指针回原处、到位后按 Y）。
       holding = 光标上还拿着物品（上一次没放成）：这时点物品区 = 放下（A），点别处不动
       Whether the drag-and-drop release spot (the drop point where the item is lifted above the finger) is the item area: set by the front end before release. If not = dragged outside the panel = discard (pointer returns to the original spot and presses Y once in place).
              holding = the cursor is still holding an item (the last drop did not succeed): then tapping the item area = put down (A), tapping elsewhere does nothing */
    int pointer_release_item, holding; float release_x, release_y;
    /* 游戏态（没开面板）的点按 = 点游戏世界：手指没落在任何控件目标上（含落在摇杆区但没拖动）、按下到抬起不到 0.35 秒、位移不超过点按阈值
       → world_tap_seq 加一，前端看到变化调宿主 dm_world_tap（点物品名拾取 / 点 NPC 对话 / 点箱子，点空地什么都不发）。
       A tap in game state (no panel open) = tap the game world: the finger did not land on any widget target (including landing in the stick area without dragging), less than 0.35 seconds from press to lift, displacement within the tap threshold
              → world_tap_seq is incremented, and on seeing a change the front end calls the host's dm_world_tap (tap an item name to pick up / tap an NPC to talk / tap a chest; tapping empty ground sends nothing). */
    unsigned world_tap_seq; float world_tap_x, world_tap_y;
    int close_left; double close_next_s;   /* 面板外点按后还要按几下 B、下一下最早什么时候（按到面板关掉为止） | After a tap outside the panel, how many more B presses remain and the earliest time of the next one (press until the panel is closed) */
    /* 商人页：这次按下落在货架的货上（前端按宿主命中结果 7 置位）。从货上起拖 = 想买：不发「拿起」、光标留在那件货上（宿主买的是光标选中的那件）；
       松手时 shop_drop_seq 加一，前端按落点决定——落在背包类物品区 = 买，拖回货架 / 面板外 / 别处 = 不买
       Vendor page: this press landed on goods on the shelf (set by the front end on host hit result 7). A drag starting from goods = intent to buy: do not send "pick up", and the cursor stays on that item (the host buys the item the cursor has selected);
              on release shop_drop_seq is incremented and the front end decides by the drop point -- landing in an inventory-type item area = buy; dragged back to the shelf / outside the panel / elsewhere = do not buy */
    int pointer_shop; unsigned shop_drop_seq;
    int pointer_lock;   /* 单件任务槽里有东西时按在别的物品上（宿主命中结果 10）：照物品区移光标出说明，但不拿起、双击 / 长按 / 单击都不动它，也不把槽里那件放过来 | Pressing on another item while the single-item quest slot holds something (host hit result 10): move the cursor and show the tooltip as in the item area, but do not pick it up; double tap / long press / single tap do not move it, and the item in the slot is not placed here either */
    /* 高级仓库的成叠格（前端按宿主命中结果 8 置位）：别的都照物品区，只有双击不同——stack_take_seq 加一，前端调宿主 dm_stash_take(1) 取 1 件到背包 | Stacked cell of the advanced stash (set by the front end on host hit result 8): everything else follows the item area, only double tap differs -- stack_take_seq is incremented and the front end calls the host's dm_stash_take(1) to take 1 into the inventory */
    int pointer_stack; unsigned stack_take_seq;
    unsigned ident_seq;   /* 物品区双击了一件未鉴定物品的次数（只有未鉴定时双击 = 免费鉴定，各页都先判这一条）：前端看到变化调宿主 dm_ui_identify_hovered | Number of double taps on an unidentified item in the item area (only when unidentified does double tap = free identify, and this rule is checked first on every page): on seeing a change the front end calls the host's dm_ui_identify_hovered */
    unsigned use_seq;   /* 物品区双击的次数（没开箱子时）：前端看到变化调宿主 dm_ui_use_item——按光标下物品走原生的使用 / 购买 / 装备，不等「按住 A」的门槛 | Number of double taps in the item area (when no stash is open): on seeing a change the front end calls the host's dm_ui_use_item -- performs the native use / buy / equip on the item under the cursor without waiting for the "hold A" threshold */
    unsigned item_tap_seq;   /* 物品区单击的次数（前端看到变化调宿主 dm_ui_item_tap：鉴定态下单击物品 = 鉴定它） | Number of single taps in the item area (on seeing a change the front end calls the host's dm_ui_item_tap: in identify state, single-tapping an item = identify it) */
    unsigned long_press_seq;   /* 物品区按住不动满长按时长的次数（前端看到变化调宿主 dm_ui_long_press：背包药水 → 腰带） | Number of times the item area was held still for the full long-press duration (on seeing a change the front end calls the host's dm_ui_long_press: inventory potion → belt) */
    unsigned tab_tap_seq;   /* 点按落在页签上的次数（前端看到变化调宿主 dm_ui_tab_go） | Number of taps that landed on a tab (on seeing a change the front end calls the host's dm_ui_tab_go) */
    int mouse_click_mode; unsigned mouse_click_seq; float mouse_click_x, mouse_click_y;
    /* 前端临时改写「面板内非物品区的点按」：非 0 = 不发鼠标点击，改成手柄光标走到触点、到位后按这个键（技能绑定流程里点技能 = 光标到它身上按 Y）。页签 / 面板外照旧 | Temporary front-end rewrite of "taps inside a panel outside the item area": non-zero = send no mouse click; instead the controller cursor walks to the touch point and presses this key on arrival (tapping a skill in the skill binding flow = cursor onto it and press Y). Tabs / outside the panel behave as before */
    uint64_t tap_button; float tap_x, tap_y; int tap_move; unsigned tap_move_seq;   /* tap_move：本次点按只把光标移到 tap_x / tap_y，不发点击（技能图标 / 属性「+」：先选中、再确认） | tap_move: this tap only moves the cursor to tap_x / tap_y and sends no click (skill icon / attribute "+": select first, then confirm) */
    double pulse_hold_s;
    double last_tap_s; float last_tap_x, last_tap_y;
    double stash_x_check_s; float stash_x_x, stash_x_y; unsigned stash_x_id;   /* 开着箱子双击背包物品先发了 X：到这个时刻那件还在光标下（游戏没收这下 X）就退回按住 Y | With the stash open, double-tapping an inventory item sent X first: if at this time the item is still under the cursor (the game did not accept that X), fall back to hold Y */
    uint64_t pointer_pending_button; double pointer_pending_hold;
    /* 难度：diff_open = 选择条展开；diff_request = 刚选中的人数档（1–8），前端交给宿主 dm_hud_request_players 后清零 | Difficulty: diff_open = selector bar expanded; diff_request = the player-count level just selected (1–8), cleared after the front end hands it to the host's dm_hud_request_players */
    int diff_open, diff_request;
    /* 腰带展开（原版键鼠「显示腰带」的那几排，手柄版没有入口）：身上的腰带多于 1 排时，在腰带格上向上滑 = 展开、向下滑 / 点别处 = 收起，
       展开后点上面几排的药 = 喝。belt_rows / belt_open 由前端每次触摸前按宿主读数写入；belt_request 前端取走后清零：
       1 展开，2 收起，0x100 | 排 << 4 | 列 = 喝那一格（排 1–3 = 底排之上第几排）
       Belt expansion (the rows of the original game's keyboard-and-mouse "show belt"; the controller version has no entry for it): when the worn belt has more than 1 row, swiping up on a belt cell = expand, swiping down / tapping elsewhere = collapse,
              and after expansion tapping a potion in the upper rows = drink. belt_rows / belt_open are written by the front end from host readings before each touch; belt_request is cleared after the front end takes it:
              1 expand, 2 collapse, 0x100 | row << 4 | column = drink that cell (row 1–3 = which row above the bottom row) */
    int belt_rows, belt_open, belt_request;
    /* 腰带点一下 = 自己喝，松手那一刻就喝，点几下喝几瓶；不判双击、腰带上不做喂雇佣兵的手势 | one tap on the belt = the player drinks, at the moment of release, several taps drink several; no double-tap detection, no mercenary feeding gesture on the belt */
    int belt_gap_cell; double belt_gap_at;   /* 按着第二组技能时要喝药：先把 ZL 和技能键松开、空出 DM_TC_ZL_LEAD_S，到 belt_gap_at 才按腰带键（belt_gap_cell = 等着的那一格，0 = 没有） | to drink a potion while a skill of the second skill set is held: first release ZL and the skill key and leave a gap of DM_TC_ZL_LEAD_S, and only at belt_gap_at press the belt key (belt_gap_cell = the cell that is waiting, 0 = none) */
    double joy_lost_s; float joy_lost_x, joy_lost_y, joy_lost_ox, joy_lost_oy;   /* 摇杆手指被系统取消的时刻与当时的位置（0 = 没有）：紧接着在附近重新落下的手指接着当摇杆 | time and position at which the joystick finger was cancelled by the system (0 = none): a finger landing again nearby right afterwards continues as the joystick */
    int joy_block;   /* 讲话文字框单独开着（前端不进面板态）：这时落在底座上的手指不起摇杆，照旧只当一次点击（关讲话框）。前端按面板栈写 | speech text box open on its own (the front end does not enter panel state): a finger on the base starts no stick and is still just one click (closes the speech box). Written by the front end from the panel stack */
    float joy_touch_x, joy_touch_y, joy_lost_tx, joy_lost_ty;   /* 摇杆那根手指的落点（方向与幅度一律按「手指相对落点」算；joy_origin 只是底盘画在哪 = 落点夹回屏内之后）；被系统取消时记下的落点 | touch point of the stick finger (direction and magnitude are always computed from "finger relative to the touch point"; joy_origin is only where the base is drawn = the touch point clamped onto the screen); the touch point recorded when it was cancelled by the system */
    double belt_pulse_s;   /* 最近一次「给玩家喝」的腰带脉冲还在发的时刻：其间按着的第二组技能连同 ZL 一起让开，收回后 ZL 重新领先 DM_TC_ZL_LEAD_S 技能键才回来 | Time until which the most recent "player drinks" belt pulse is still being sent: during it the held second-set skill gives way together with ZL, and after the release ZL again leads by DM_TC_ZL_LEAD_S before the skill key comes back */
    int belt_q;   /* 此刻有别的脉冲在发 / 别的补发在等：排着，空了再落地（不互相盖掉）。值 = 格 | another pulse is being sent / another resend is pending: queue and fire when free (they never overwrite each other). Value = cell */
} dm_tc_state;

/* 原版UI消息驱动，不根据输入键猜面板。AutoMap是战斗叠加，不切导航。 | Driven by the original game's UI messages; panels are not guessed from input keys. AutoMap is a combat overlay and does not switch to navigation. */
typedef struct { int game; uint64_t panels; } dm_tc_scene;
int dm_tc_navigation(dm_tc_scene *scene, const char *cat, const char *act, const char *arg);
int dm_tc_scene_dialog_only(const dm_tc_scene *scene);   /* 只开着 NPC 对话菜单 | Only the NPC dialog menu is open */
int dm_tc_scene_dead(void);   /* 死亡弹窗开着（导航态，但不给手柄圆钮） | Death popup open (navigation state, but no controller round buttons) */
/* 面板模式（前端两端共用一个算式，iOS front_ios.m 与 Mac touch_mac.c）：0 = 游戏态；1 = 面板态；2 = 死亡页；3 = 只开着 NPC 对话菜单。
   nav = 手势层的导航判断（dm_tc_navigation，再并上「面板栈里还开着非常驻面板」）；blocked = 面板栈读数（dm_hud_stack_msg：只开着对话菜单时 -1）
   Panel mode (one formula shared by both front ends, iOS front_ios.m and Mac touch_mac.c): 0 = game; 1 = panel; 2 = death screen; 3 = only the NPC dialog menu open.
      nav = the gesture layer's navigation judgment (dm_tc_navigation, combined with "a non-resident panel is still open in the panel stack"); blocked = the panel stack reading (dm_hud_stack_msg: -1 when only the dialog menu is open) */
int dm_tc_panel_mode(int nav, const dm_tc_scene *scene, int blocked);

/* 坐标使用 UIKit 点；布局本身不画球、不声称游戏已绑定回城/腰带。 | Coordinates use UIKit points; the layout itself does not draw the orbs and does not claim the game has bound town portal/belt. */
/* 安全区左右内缩（点）；在 init / resize 之前设，之后的布局按它把右侧技能键组与左摇杆移进安全区。缺省 0。 | Left/right safe-area insets (points); set before init / resize, and the layout afterwards uses them to move the right-side skill key group and the left stick into the safe area. Default 0. */
void dm_tc_set_safe_insets(float left, float right);
void dm_tc_set_edge_bottom(float bottom);   /* 屏幕底边归系统的那一条的高度（点）；0 = 没有。只约束移动摇杆的起手 | Height (points) of the strip along the bottom edge of the screen that belongs to the system; 0 = none. Constrains only where the movement stick may start */
void dm_tc_layout_make(dm_tc_layout *layout, float width, float height);
int dm_tc_hit(const dm_tc_layout *layout, float x, float y);
#define DM_TC_POINTER_LIFT_PT 40.0f   // 功能页拖动时物品抬在手指上方的点数（× 控件缩放） | Number of points the item is lifted above the finger while dragging on function pages (× widget scale)
void dm_tc_set_swap_above(int above);   // 0：传送门左侧外弧，1：技能区上方；修改后调用 resize | 0: left of portal on outer arc, 1: above skill fan; call resize after changing.
void dm_tc_set_safe_bottom(float bottom);
void dm_tc_set_tall_scale(int on);   // 平板缩放开关，上限 1.15；Mac 像素回放关闭 | Tablet scale opt-in, capped at 1.15; off for Mac pixel replay.
float dm_tc_scale_for_size(float width, float height);   // 按点数计算控件比例，不随平板高度增长 | Point-based control scale; independent of tablet height.
void dm_tc_init(dm_tc_state *state, float width, float height);
/* 尺寸变更会取消全部触点，调用者随后推送归零状态。 | A size change cancels all touch points, and the caller then pushes the zeroed state. */
void dm_tc_resize(dm_tc_state *state, float width, float height);
/* 控件开关不改变布局；切换有效位时取消已有触点。 | Widget switches do not change the layout; existing touch points are cancelled when the valid bits are toggled. */
void dm_tc_enable_targets(dm_tc_state *state, uint32_t mask);
/* 导航模式（面板 / 对话 / 前端页开着）下可用的控件：右侧五个虚拟手柄圆钮（确认 / 返回 / 操作 / 绑定 / 下一页 = A B X Y R）。
   底栏十字位置的「关闭」不再给：面板的关闭入口只留一个（外框右上角的关闭按钮；圆钮还在时另有「返回」）。
   pad = 0：五个圆钮也不给（局内手指直接点界面接上之后用，开关 DM_TOUCH_NAV_PAD=0，缺省 1）。两个前端共用这一处，不各写各的。
   Widgets available in navigation mode (a panel / dialog / front-end page is open): the five virtual controller round buttons on the right (confirm / back / action / bind / next page = A B X Y R).
      The "close" at the bottom bar's cross position is no longer offered: only one close entry for panels remains (the close button at the top-right corner of the frame; while the round buttons are present there is also "back").
      pad = 0: the five round buttons are not offered either (used once in-run fingers tapping the UI directly is hooked up; switch DM_TOUCH_NAV_PAD=0, default 1). Both front ends share this one place and do not each write their own. */
static inline uint32_t dm_tc_nav_mask(int pad) { return pad ? ((1u<<DM_TC_MAIN)|(1u<<DM_TC_SKILL1)|(1u<<DM_TC_SKILL2)|(1u<<DM_TC_SKILL3)|(1u<<DM_TC_SKILL4)) : 0; }
/* 模式变化释放全部手势；导航模式只按键，不启用技能拖拽和地图取消区。 | A mode change releases all gestures; navigation mode only presses keys and does not enable skill dragging or the map cancel area. */
void dm_tc_set_navigation_mode(dm_tc_state *state, int navigation_mode);
void dm_tc_set_joy_block(dm_tc_state *state, int block);   /* 讲话文字框 / 对话菜单单独开着：落在底座上的手指不起摇杆、只当一次点击 | speech text box / dialogue menu open on its own: a finger on the base starts no stick and is just one click */
/* 只取消回城控件拥有的core触点；物理触点由前端保留。 | Cancel only the core touch point owned by the town-portal widget; the physical touch point is kept by the front end. */
void dm_tc_set_portal_button(dm_tc_state *state, uint64_t mask);
void dm_tc_set_portal_binding(dm_tc_state *state, uint64_t mask, uint64_t binding_identity, uint64_t scene_epoch);
/* 反馈确认仅作用于仍持有且身份/场景/请求一致的pending手势；返回是否首次armed。 | Feedback confirmation applies only to a pending gesture that is still held and whose identity/scene/request match; returns whether it was armed for the first time. */
int dm_tc_portal_ack(dm_tc_state *state, uint64_t gesture_epoch, uint64_t binding_identity, uint64_t scene_epoch);
void dm_tc_cancel_portal(dm_tc_state *state);
void dm_tc_begin(dm_tc_state *state, int slot, float x, float y);
void dm_tc_move(dm_tc_state *state, int slot, float x, float y);
void dm_tc_end(dm_tc_state *state, int slot);
void dm_tc_begin_at(dm_tc_state *state, int slot, float x, float y, double now_s);
void dm_tc_move_at(dm_tc_state *state, int slot, float x, float y, double now_s);
void dm_tc_end_at(dm_tc_state *state, int slot, double now_s);
void dm_tc_tick(dm_tc_state *state, double now_s);
void dm_tc_cancel_all(dm_tc_state *state);
void dm_tc_cancel_finger(dm_tc_state *state, int slot);   /* 系统取消这根手指：不当松手，只放掉它按着的东西 | The system cancels this finger: do not treat it as a release, only let go of what it was holding */
/* 结束 epoch 对应的施放脉冲；返回是否有变化（调用者随后推送） | End the cast pulse corresponding to epoch; returns whether anything changed (the caller then pushes) */
int dm_tc_pulse_end(dm_tc_state *state, uint64_t pulse_epoch);
// 收回后还有排着的指针脉冲（拖放的「放下」在「拿起」没收时就到了）：前端等游戏看到一次松开后调 resume 开始它
// After release there is still a queued pointer pulse (the "put-down" of a drag-and-drop arrived before the "pick-up" was released): the front end waits until the game has seen a release once, then calls resume to start it
int dm_tc_pulse_pending(const dm_tc_state *state);
void dm_tc_pointer_press(dm_tc_state *state, float x, float y, uint64_t button);   /* 光标到 (x, y) 再按一下 button；当前指针手指记为「长按已处理」，松手不再算点按 | Cursor to (x, y), then press button once; the current pointer finger is marked "long press handled" and its release no longer counts as a tap */
void dm_tc_pointer_item(dm_tc_state *state, int kind);   /* 指针手指的命中结果一出来就调（最晚松手前）：1 = 物品区，2 = 页签，3 = 面板外（点按 = 关面板），其余 = 面板内别处（点按 = 鼠标点击） | Called as soon as the pointer finger's hit result is available (before release at the latest): 1 = item area, 2 = tab, 3 = outside the panel (tap = close the panel), anything else = elsewhere inside the panel (tap = mouse click) */
void dm_tc_pointer_release(dm_tc_state *state, int kind);   /* 拖放松手前调：宿主的落点判定（1 放在抬起点 / 4 放在手指点 / 3 丢弃 / 0 放回原处） | Called before a drag-and-drop release: the host's drop decision (1 place at the lifted point / 4 place at the finger point / 3 discard / 0 put back where it was) */
void dm_tc_pointer_release_at(dm_tc_state *state, float x, float y);   /* 拖放松手前调：放在这个位置（宿主算的，装备槽 = 槽心） | Called before a drag-and-drop release: place at this position (computed by the host; equipment slot = slot center) */
void dm_tc_set_holding(dm_tc_state *state, int holding);
void dm_tc_set_cur_stack(unsigned id);   /* 仓库开着时光标此刻指着的背包物品的 id（指着仓库一侧 / 没指着 = 0） | id of the inventory item the cursor is pointing at right now while the stash is open (pointing at the stash side / pointing at nothing = 0) */
void dm_tc_set_hover_unid(unsigned id);   /* 光标此刻指着的我方未鉴定物品 id（宿主 dm_ui_hover_unid）：物品区双击先判它——非 0 = 鉴定（ident_seq 加一），不走各页原来的双击 | id of our unidentified item the cursor is pointing at right now (host dm_ui_hover_unid): an item-area double tap checks it first -- non-zero = identify (ident_seq incremented), skipping each page's original double tap */
void dm_tc_set_tap_box(int on);   /* 最近一次单击的物品是不是魔盒（开着箱子时双击魔盒 = 打开） | Whether the most recently single-tapped item is the cube (with the stash open, double-tapping the cube = open) */
void dm_tc_set_panel_state(int bits);   /* 宿主读游戏面板树得到的状态位（1 储藏箱开着 / 2 商人页 / 4 死亡弹窗）；界面消息 DM:PanelState 到来时前端调 | State bits the host obtains by reading the game panel tree (1 stash open / 2 vendor page / 4 death popup); the front end calls this when the UI message DM:PanelState arrives */
void dm_tc_set_iso_fix(int on);   /* 移动摇杆斜向预修正开关（缺省开；测量改前改后用） | Switch for the movement stick's diagonal pre-correction (default on; used to measure before and after the change) */
int dm_tc_belt_outside(const dm_tc_state *state, float x, float y);
#define DM_TC_AIM_SETTLE_S 0.08    /* 技能钮按下后等这么久判定是点按还是拖动（拖过死区就提前键下） | After a skill button is pressed, wait this long to decide tap versus drag (dragging past the dead zone puts the key down early) */
#define DM_TC_BELT_SWIPE_PT 24      /* 腰带格上松手时纵向位移过这么多点 = 展开 / 收起；不到 = 点按（喝） | On release over a belt cell, vertical displacement beyond this many points = expand / collapse; less = tap (drink) */
#define DM_TC_KEY_MIN_S 0.035       /* 技能键至少按住这么久（约 2 帧）；不够的松手时补成脉冲，保证游戏读到 | A skill button is held at least this long (about 2 frames); a shorter one is made up as a pulse on release to guarantee the game reads it */
#define DM_TC_JOY_RESUME_S 0.35   /* 摇杆那根手指被系统取消后，这么久之内在原处附近重新落下的手指接着当摇杆（不要求落在底座附近） | after the joystick finger is cancelled by the system, a finger landing again near the same spot within this long continues as the joystick (landing near the base is not required) */
#define DM_TC_JOY_GRAB_K 1.5f   /* 摇杆那根手指被系统取消后重新落下：离原处这么多倍底盘半径之内才接着当摇杆 | after the stick finger is cancelled by the system and touches down again: it continues as the stick only within this many base radii of the original place */
#define DM_TC_ZL_LEAD_S 0.17   /* 修饰键领先主键的时间（约 10 帧；同一帧按下游戏只认主键） | Time the modifier key leads the main key (about 10 frames; pressed in the same frame, the game only recognizes the main key) */
#define DM_TC_DOUBLE_TAP_S 0.40   /* 两次单击间隔上限 | Upper limit of the interval between two single taps */
#define DM_TC_HOLD_USE_S 0.60     /* 「按住」动作的按住时长：Mac 宿主实测 0.25 s 只捡起、0.45 s 打开魔盒 | Hold duration of the "hold" action: measured on the Mac host, 0.25 s only picks up, 0.45 s opens the cube */
int dm_tc_pulse_resume(dm_tc_state *state);

#ifdef __cplusplus
}
#endif
