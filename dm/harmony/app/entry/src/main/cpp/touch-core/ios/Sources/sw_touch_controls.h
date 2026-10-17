#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SW_TC_MAX_TOUCH = 16, SW_TC_COUNT = 12 };
enum {
    SW_TC_NONE = -1, SW_TC_JOYSTICK = -2, SW_TC_SWAP = -3, SW_TC_DIFF = -4,   /* DIFF：难度格 / 选择条占用的手指，不发按键 */
    SW_TC_EDGE = -5,   /* 屏幕底边归系统的那一条里起手的手指：不当摇杆，松手也不当点地 */
    SW_TC_MAIN = 0,
    SW_TC_SKILL1, SW_TC_SKILL2, SW_TC_SKILL3, SW_TC_SKILL4,
    SW_TC_PORTAL, SW_TC_BELT1, SW_TC_BELT2, SW_TC_BELT3, SW_TC_BELT4,
    SW_TC_MENU, SW_TC_MAP
};
typedef struct { float x, y, r; uint64_t button; } sw_tc_target;
typedef struct {
    float width, height;
    sw_tc_target target[SW_TC_COUNT];
    float joy_x, joy_y, joy_radius;   /* 没按时底盘的显示位置（静止位）与底盘半径 */
    float joy_zone_r, joy_zone_t, joy_min_x, joy_max_y;   /* 起摇杆区的右界与上界（左与下到屏幕边）；按下后底盘圆心的夹回界：x 不小于、y 不大于 */
    sw_tc_target swap;   /* 技能组切换键：本地开关，不直接发按键 */
    sw_tc_target diff;   /* 底部 UI v4 第 0 格难度：点开 / 收起 1–8 选择条，不发按键 */
    sw_tc_target pick[8];   /* 选择条 8 格（第 i 格 = 人数档 i+1），在七格上方 */
} sw_tc_layout;
typedef struct {
    int active, target;
    float x, y;
    float aim_origin_x, aim_origin_y;
    uint64_t aim_move_sequence;
    int aim_cancelled;
    int key_down; double key_down_s;   /* 技能钮：键是否已按下过、几时按下的（按下后先等判定时间，见 update） */
    double start_time_s;
    int pointer_dragged, pointer_long;
    /* 界面连续手势（鼠标点击模式、起手不在物品区也不在面板外、位移过阈值时按起始方向定型，定型后不变）：1 = 纵向拖（滚动），2 = 横向拖（滑块）。
       手势层只定型，前端看到后把起手点与当前点转给宿主（sw_ui_drag_*），滚动 / 滑块由宿主按游戏自己的命中去做 */
    int ui_drag;
    int shop_drag;   /* 从商人货架的货上起拖（不拿起）：松手按落点买 / 不买 */
    int locked_drag; /* 单件任务槽占用时从别的物品上起拖：不拿起，松手不发任何键 */
} sw_tc_finger;
typedef struct {
    sw_tc_layout layout;
    sw_tc_finger finger[SW_TC_MAX_TOUCH];
    int joy_slot;
    float joy_origin_x, joy_origin_y;
    uint32_t enabled_targets;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    int aim_slot, navigation_mode, map_cancel_active;
    uint64_t aim_move_sequence;
    int portal_pending, portal_phase; // 0idle / 1modifier-pending / 2armed
    uint64_t gesture_epoch, portal_binding_identity, portal_scene_epoch;
    /* 松手施放的短按脉冲：按键与右轴方向同时发布，前端到时调 sw_tc_pulse_end 收回 */
    uint64_t pulse_button, pulse_epoch;
    int32_t pulse_rx, pulse_ry;
    uint64_t defer_button; double defer_at_s;   /* 第二组里点得太短的那一下：等 ZL 领先够了再补发 */
    /* 技能组：0 = 第一套；1 = 第二套（原版 Switch 按住 ZL 用 alt 槽）。是模式，不随取消/尺寸变化清零 */
    int skill_set;
    /* 第二组技能（ZL 修饰）：ZL 只在手指按着主钮 / 技能钮、或技能脉冲在发时按着，且先于技能键 SW_TC_ZL_LEAD_S 按下。
       松手施放按得太短时，这一下脉冲延到 ZL 领先够了再发（sw_tc_tick 到点起）。 */
    int pointer_slot;
    double last_time_s;
    int pointer_pickup_done;
    int pointer_pulse_active, pointer_pulse_pending;
    float pointer_pending_x, pointer_pending_y;
    int pointer_valid;
    float pointer_x, pointer_y;
    /* 物品手势：pointer_item = 当前指针手指落在物品区（前端在松手前按宿主命中结果置位）。物品区单击只移光标（出说明）、
       双击 = 按住 A（游戏提示：按住 = 打开 / 使用 / 装备；开着箱子时按住 Y = 移到另一侧）；非物品区单击照旧 = 移光标 + 按 A。
       pulse_hold_s > 0 = 这个脉冲要按住这么久，前端在「游戏已取走」之外再等满它才收 */
    int pointer_item;
    /* 非物品区的点按走鼠标点击（暂停菜单按钮、布局里插回的关闭 / 修理等按钮只认鼠标）：mouse_click_mode 由前端打开；
       每次这样的点按 mouse_click_seq 加一，前端看到变化就在 (mouse_click_x, mouse_click_y) 发一次界面鼠标点击，不再按 A */
    /* 拖放松手处（物品抬在手指上方的那个落点）是不是物品区：前端松手前置位。不是 = 拖到面板外 = 丢弃（指针回原处、到位后按 Y）。
       holding = 光标上还拿着物品（上一次没放成）：这时点物品区 = 放下（A），点别处不动 */
    int pointer_release_item, holding; float release_x, release_y;
    /* 游戏态（没开面板）的点按 = 点游戏世界：手指没落在任何控件目标上（含落在摇杆区但没拖动）、按下到抬起不到 0.35 秒、位移不超过点按阈值
       → world_tap_seq 加一，前端看到变化调宿主 sw_world_tap（点物品名拾取 / 点 NPC 对话 / 点箱子，点空地什么都不发）。 */
    unsigned world_tap_seq; float world_tap_x, world_tap_y;
    int close_left; double close_next_s;   /* 面板外点按后还要按几下 B、下一下最早什么时候（按到面板关掉为止） */
    /* 商人页：这次按下落在货架的货上（前端按宿主命中结果 7 置位）。从货上起拖 = 想买：不发「拿起」、光标留在那件货上（宿主买的是光标选中的那件）；
       松手时 shop_drop_seq 加一，前端按落点决定——落在背包类物品区 = 买，拖回货架 / 面板外 / 别处 = 不买 */
    int pointer_shop; unsigned shop_drop_seq;
    int pointer_lock;   /* 单件任务槽里有东西时按在别的物品上（宿主命中结果 10）：照物品区移光标出说明，但不拿起、双击 / 长按 / 单击都不动它，也不把槽里那件放过来 */
    /* 高级仓库的成叠格（前端按宿主命中结果 8 置位）：别的都照物品区，只有双击不同——stack_take_seq 加一，前端调宿主 sw_stash_take(1) 取 1 件到背包 */
    int pointer_stack; unsigned stack_take_seq;
    unsigned ident_seq;   /* 物品区双击了一件未鉴定物品的次数（只有未鉴定时双击 = 免费鉴定，各页都先判这一条）：前端看到变化调宿主 sw_ui_identify_hovered */
    unsigned use_seq;   /* 物品区双击的次数（没开箱子时）：前端看到变化调宿主 sw_ui_use_item——按光标下物品走原生的使用 / 购买 / 装备，不等「按住 A」的门槛 */
    unsigned item_tap_seq;   /* 物品区单击的次数（前端看到变化调宿主 sw_ui_item_tap：鉴定态下单击物品 = 鉴定它） */
    unsigned long_press_seq;   /* 物品区按住不动满长按时长的次数（前端看到变化调宿主 sw_ui_long_press：背包药水 → 腰带） */
    unsigned tab_tap_seq;   /* 点按落在页签上的次数（前端看到变化调宿主 sw_ui_tab_go） */
    int mouse_click_mode; unsigned mouse_click_seq; float mouse_click_x, mouse_click_y;
    /* 前端临时改写「面板内非物品区的点按」：非 0 = 不发鼠标点击，改成手柄光标走到触点、到位后按这个键（技能绑定流程里点技能 = 光标到它身上按 Y）。页签 / 面板外照旧 */
    uint64_t tap_button; float tap_x, tap_y; int tap_move; unsigned tap_move_seq;   /* tap_move：本次点按只把光标移到 tap_x / tap_y，不发点击（技能图标 / 属性「+」：先选中、再确认） */
    double pulse_hold_s;
    double last_tap_s; float last_tap_x, last_tap_y;
    double stash_x_check_s; float stash_x_x, stash_x_y; unsigned stash_x_id;   /* 开着箱子双击背包物品先发了 X：到这个时刻那件还在光标下（游戏没收这下 X）就退回按住 Y */
    uint64_t pointer_pending_button; double pointer_pending_hold;
    /* 难度：diff_open = 选择条展开；diff_request = 刚选中的人数档（1–8），前端交给宿主 sw_hud_request_players 后清零 */
    int diff_open, diff_request;
    /* 腰带展开（原版键鼠「显示腰带」的那几排，手柄版没有入口）：身上的腰带多于 1 排时，在腰带格上向上滑 = 展开、向下滑 / 点别处 = 收起，
       展开后点上面几排的药 = 喝。belt_rows / belt_open 由前端每次触摸前按宿主读数写入；belt_request 前端取走后清零：
       1 展开，2 收起，0x100 | 排 << 4 | 列 = 喝那一格（排 1–3 = 底排之上第几排） */
    int belt_rows, belt_open, belt_request;
    /* 腰带点一下 = 自己喝，松手那一刻就喝，点几下喝几瓶；不判双击、腰带上不做喂雇佣兵的手势 */
    int belt_gap_cell; double belt_gap_at;   /* 按着第二组技能时要喝药：先把 ZL 和技能键松开、空出 SW_TC_ZL_LEAD_S，到 belt_gap_at 才按腰带键（belt_gap_cell = 等着的那一格，0 = 没有） */
    double joy_lost_s; float joy_lost_x, joy_lost_y, joy_lost_ox, joy_lost_oy;   /* 摇杆手指被系统取消的时刻与当时的位置（0 = 没有）：紧接着在附近重新落下的手指接着当摇杆 */
    int joy_block;   /* 讲话文字框单独开着（前端不进面板态）：这时落在底座上的手指不起摇杆，照旧只当一次点击（关讲话框）。前端按面板栈写 */
    double belt_pulse_s;   /* 最近一次「给玩家喝」的腰带脉冲还在发的时刻：其间按着的第二组技能连同 ZL 一起让开，收回后 ZL 重新领先 SW_TC_ZL_LEAD_S 技能键才回来 */
    int belt_q;   /* 此刻有别的脉冲在发 / 别的补发在等：排着，空了再落地（不互相盖掉）。值 = 格 */
} sw_tc_state;

/* 原版UI消息驱动，不根据输入键猜面板。AutoMap是战斗叠加，不切导航。 */
typedef struct { int game; uint64_t panels; } sw_tc_scene;
int sw_tc_navigation(sw_tc_scene *scene, const char *cat, const char *act, const char *arg);
int sw_tc_scene_dialog_only(const sw_tc_scene *scene);   /* 只开着 NPC 对话菜单 */
int sw_tc_scene_dead(void);   /* 死亡弹窗开着（导航态，但不给手柄圆钮） */

/* 坐标使用 UIKit 点；布局本身不画球、不声称游戏已绑定回城/腰带。 */
/* 安全区左右内缩（点）；在 init / resize 之前设，之后的布局按它把右侧技能键组与左摇杆移进安全区。缺省 0。 */
void sw_tc_set_safe_insets(float left, float right);
void sw_tc_set_edge_bottom(float bottom);   /* 屏幕底边归系统的那一条的高度（点）；0 = 没有。只约束移动摇杆的起手 */
void sw_tc_layout_make(sw_tc_layout *layout, float width, float height);
int sw_tc_hit(const sw_tc_layout *layout, float x, float y);
#define SW_TC_POINTER_LIFT_PT 40.0f   // 功能页拖动时物品抬在手指上方的点数（× 控件缩放）
void sw_tc_set_tall_scale(int on);   // 画面高 > 440 点时按 高/440 放大的开关；缺省关（Mac 宿主触屏用例传的是像素数，不能放大），iOS 前端建视图时开
float sw_tc_scale_for_size(float width, float height);   // 控件缩放：手机 min(1, 宽/667, 高/375)；画面高 > 440 点（iPad）时按 高/440 等比放大
void sw_tc_init(sw_tc_state *state, float width, float height);
/* 尺寸变更会取消全部触点，调用者随后推送归零状态。 */
void sw_tc_resize(sw_tc_state *state, float width, float height);
/* 控件开关不改变布局；切换有效位时取消已有触点。 */
void sw_tc_enable_targets(sw_tc_state *state, uint32_t mask);
/* 导航模式（面板 / 对话 / 前端页开着）下可用的控件：右侧五个虚拟手柄圆钮（确认 / 返回 / 操作 / 绑定 / 下一页 = A B X Y R）。
   底栏十字位置的「关闭」不再给：面板的关闭入口只留一个（外框右上角的关闭按钮；圆钮还在时另有「返回」）。
   pad = 0：五个圆钮也不给（局内手指直接点界面接上之后用，开关 SW_TOUCH_NAV_PAD=0，缺省 1）。两个前端共用这一处，不各写各的。 */
static inline uint32_t sw_tc_nav_mask(int pad) { return pad ? ((1u<<SW_TC_MAIN)|(1u<<SW_TC_SKILL1)|(1u<<SW_TC_SKILL2)|(1u<<SW_TC_SKILL3)|(1u<<SW_TC_SKILL4)) : 0; }
/* 模式变化释放全部手势；导航模式只按键，不启用技能拖拽和地图取消区。 */
void sw_tc_set_navigation_mode(sw_tc_state *state, int navigation_mode);
void sw_tc_set_joy_block(sw_tc_state *state, int block);   /* 讲话文字框 / 对话菜单单独开着：落在底座上的手指不起摇杆、只当一次点击 */
/* 只取消回城控件拥有的core触点；物理触点由前端保留。 */
void sw_tc_set_portal_button(sw_tc_state *state, uint64_t mask);
void sw_tc_set_portal_binding(sw_tc_state *state, uint64_t mask, uint64_t binding_identity, uint64_t scene_epoch);
/* 反馈确认仅作用于仍持有且身份/场景/请求一致的pending手势；返回是否首次armed。 */
int sw_tc_portal_ack(sw_tc_state *state, uint64_t gesture_epoch, uint64_t binding_identity, uint64_t scene_epoch);
void sw_tc_cancel_portal(sw_tc_state *state);
void sw_tc_begin(sw_tc_state *state, int slot, float x, float y);
void sw_tc_move(sw_tc_state *state, int slot, float x, float y);
void sw_tc_end(sw_tc_state *state, int slot);
void sw_tc_begin_at(sw_tc_state *state, int slot, float x, float y, double now_s);
void sw_tc_move_at(sw_tc_state *state, int slot, float x, float y, double now_s);
void sw_tc_end_at(sw_tc_state *state, int slot, double now_s);
void sw_tc_tick(sw_tc_state *state, double now_s);
void sw_tc_cancel_all(sw_tc_state *state);
void sw_tc_cancel_finger(sw_tc_state *state, int slot);   /* 系统取消这根手指：不当松手，只放掉它按着的东西 */
/* 结束 epoch 对应的施放脉冲；返回是否有变化（调用者随后推送） */
int sw_tc_pulse_end(sw_tc_state *state, uint64_t pulse_epoch);
// 收回后还有排着的指针脉冲（拖放的「放下」在「拿起」没收时就到了）：前端等游戏看到一次松开后调 resume 开始它
int sw_tc_pulse_pending(const sw_tc_state *state);
void sw_tc_pointer_press(sw_tc_state *state, float x, float y, uint64_t button);   /* 光标到 (x, y) 再按一下 button；当前指针手指记为「长按已处理」，松手不再算点按 */
void sw_tc_pointer_item(sw_tc_state *state, int kind);   /* 指针手指的命中结果一出来就调（最晚松手前）：1 = 物品区，2 = 页签，3 = 面板外（点按 = 关面板），其余 = 面板内别处（点按 = 鼠标点击） */
void sw_tc_pointer_release(sw_tc_state *state, int kind);   /* 拖放松手前调：宿主的落点判定（1 放在抬起点 / 4 放在手指点 / 3 丢弃 / 0 放回原处） */
void sw_tc_pointer_release_at(sw_tc_state *state, float x, float y);   /* 拖放松手前调：放在这个位置（宿主算的，装备槽 = 槽心） */
void sw_tc_set_holding(sw_tc_state *state, int holding);
void sw_tc_set_cur_stack(unsigned id);   /* 仓库开着时光标此刻指着的背包物品的 id（指着仓库一侧 / 没指着 = 0） */
void sw_tc_set_hover_unid(unsigned id);   /* 光标此刻指着的我方未鉴定物品 id（宿主 sw_ui_hover_unid）：物品区双击先判它——非 0 = 鉴定（ident_seq 加一），不走各页原来的双击 */
void sw_tc_set_tap_box(int on);   /* 最近一次单击的物品是不是魔盒（开着箱子时双击魔盒 = 打开） */
void sw_tc_set_panel_state(int bits);   /* 宿主读游戏面板树得到的状态位（1 储藏箱开着 / 2 商人页 / 4 死亡弹窗）；界面消息 SW:PanelState 到来时前端调 */
void sw_tc_set_iso_fix(int on);   /* 移动摇杆斜向预修正开关（缺省开；测量改前改后用） */
int sw_tc_belt_outside(const sw_tc_state *state, float x, float y);
#define SW_TC_AIM_SETTLE_S 0.08    /* 技能钮按下后等这么久判定是点按还是拖动（拖过死区就提前键下） */
#define SW_TC_BELT_SWIPE_PT 24      /* 腰带格上松手时纵向位移过这么多点 = 展开 / 收起；不到 = 点按（喝） */
#define SW_TC_KEY_MIN_S 0.035       /* 技能键至少按住这么久（约 2 帧）；不够的松手时补成脉冲，保证游戏读到 */
#define SW_TC_JOY_RESUME_S 0.35   /* 摇杆那根手指被系统取消后，这么久之内在原处附近重新落下的手指接着当摇杆（不要求落在底座附近） */
#define SW_TC_JOY_GRAB_K 1.5f   /* 摇杆那根手指被系统取消后重新落下：离原处这么多倍底盘半径之内才接着当摇杆 */
#define SW_TC_ZL_LEAD_S 0.17   /* 修饰键领先主键的时间（约 10 帧；同一帧按下游戏只认主键） */
#define SW_TC_DOUBLE_TAP_S 0.40   /* 两次单击间隔上限 */
#define SW_TC_HOLD_USE_S 0.60     /* 「按住」动作的按住时长：Mac 宿主实测 0.25 s 只捡起、0.45 s 打开魔盒 */
int sw_tc_pulse_resume(sw_tc_state *state);

#ifdef __cplusplus
}
#endif
