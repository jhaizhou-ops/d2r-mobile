#pragma once
#include <stdint.h>

// 界面代号：游戏每重建一次界面（切手柄 / 触屏布局与运行模式的通知被游戏取走、切语言、面板卸载、前端 ↔ 局内、顶层面板换了一批）加 1。
// 重建会释放旧控件；跨帧留着的控件裸指针再读就是野指针（腰带补满崩溃），只比身份的也会被新控件复用旧地址骗过。
// 规则：游戏控件指针只在同一帧里用；要跨帧存，一律用 dm_wref——存时记代号；取时代号没变直接用，变了就核它还挂不挂在当前界面树上（只比指针、不解引用）：
// 还在 = 没被释放（卸掉的只是别的面板），记上新代号照用；不在了给 0，调用方按名字 / 类型重新找、找不到就停。
// 原先代号一变一律给 0：任何顶层面板卸掉都会让挪位表忘了小地图「已挪过」，真全屏的贴顶上提一次次叠加、把小地图推出屏外（回归 ipadfull top 局）。
// 静态扫 dm/host/tests/widget_cache_scan.py 查「静态变量裸存控件指针」，在 tests/Makefile 里跑。
// UI generation: incremented each time the game rebuilds the UI (the game consumes a controller / touch layout or run-mode notification, language switch, panel unload, front end ↔ in-game, a batch of top-level panels replaced).
// A rebuild frees old widgets; reading a raw widget pointer kept across frames is a dangling pointer (the belt-fill crash), and even identity-only comparisons can be fooled by a new widget reusing the old address.
// Rule: use game widget pointers only within the same frame; to keep one across frames, always use dm_wref, which records the generation when stored; on fetch, if the generation is unchanged use it directly, otherwise check whether it is still attached to the current UI tree (pointer comparison only, no dereference):
// still there = not freed (only some other panel was unloaded), record the new generation and keep using it; gone returns 0, and the caller re-finds it by name / type and stops if not found.
// Previously any generation change returned 0: any top-level panel unload made the move table forget the minimap had "already been moved", so the true-fullscreen snap-to-top shift stacked up again and again and pushed the minimap off screen (ipadfull top regression run).
// The static scan dm/host/tests/widget_cache_scan.py looks for "raw widget pointers stored in static variables" and runs in tests/Makefile.
uint32_t dm_ui_gen(void);                 // 当前界面代号（从 1 起）；任意线程可读 | Current UI generation (starting from 1); readable from any thread
enum { DM_UIGEN_OPMODE = 1, DM_UIGEN_PERFMODE, DM_UIGEN_LANG, DM_UIGEN_PANEL_GONE, DM_UIGEN_ROOT };
void dm_ui_gen_bump(int why);             // 界面重建了（任意线程）；why = DM_UIGEN_* | The UI was rebuilt (any thread); why = DM_UIGEN_*
int dm_ui_unstable(void);                 // 重建进行中或刚重建（2 帧内）：控件可能还没建完，这一帧别去找 / 别用 | Rebuild in progress or just finished (within 2 frames): widgets may not be fully built yet; do not look up / use them this frame
typedef struct { uint64_t w; uint32_t gen; } dm_wref;   // 跨帧存的游戏控件：只许游戏主线程读写 | Game widgets stored across frames: read/write only on the game main thread
static inline void dm_wref_set(dm_wref *r, uint64_t w) { r->w = w; r->gen = dm_ui_gen(); }
uint64_t dm_wref_get(dm_wref *r);         // 只许游戏主线程调 | Game main thread only
int dm_ui_gen_revalidate(uint32_t *gen, uint64_t anchor);   // 整组缓存（结构体带 ui_gen）：代号变了时按锚点控件还在不在树上判整组还能不能用，在就记上新代号、返回 1 | Group cache (struct with ui_gen): when the generation changes, decide whether the whole group is still usable by whether the anchor widget is still in the tree; if so, record the new generation and return 1

void dm_cursor_set(int x, int y);
// 前端登记「待写原版光标」（屏幕点坐标），由宿主在游戏读手柄时（hle_hid fill）落地：
// 与按键同一次采样到达游戏。前端主线程随时直写光标全局，游戏处理 A 之前会用自己的手柄光标状态把它写回，A 落在旧光标处
// The front end registers a 'pending write to the original game's cursor' (screen point coordinates), and the host applies it when the game reads the controller (hle_hid fill):
// it reaches the game in the same sample as the keys. If the front-end main thread writes the cursor global directly at an arbitrary time, the game writes it back from its own controller cursor state before processing A, and A lands at the old cursor
void dm_cursor_set_screen(float x, float y, float screen_w, float screen_h);
void dm_cursor_clear(void);
int dm_ui_mouse_hover_now(float nx, float ny);   // 游戏线程：Mac 无按键悬停，只更新自由光标；不点击、不吸附邻格 | Game thread: Mac button-free hover, updates the free cursor without clicking or snapping to nearby cells
// 摇杆闭环（过渡做法，原版鼠标模式开关找到前用）：真机上原版逐帧鼠标移动那条路不跑，直写的光标会被手柄光标逻辑盖回。
// 改为：每次读手柄时读原版光标 (main+0x2d128f8/fc)，离目标 > DM_CURSOR_NEAR（9）像素就推左摇杆朝目标走（60 像素内按距离减力）；
// 没到位时按下的 A 连同当时的目标位置排队扣住，光标走到该位置才放出（拖放的起点 / 终点各一下）；排队 1.5 秒没到位报红并丢掉。
// 宿主读手柄处（hle_hid fill）调用，与按键同一次采样到达游戏
// Stick closed loop (interim approach, used until the original game's mouse mode switch is found): on a real device the original game's per-frame mouse move path does not run, and a directly written cursor gets overwritten by the controller cursor logic.
// Instead: on every controller read, read the original game's cursor (main+0x2d128f8/fc), and if it is > DM_CURSOR_NEAR (9) pixels from the target push the left stick toward the target (force reduced by distance within 60 pixels);
// an A pressed before it is in position is queued and held back together with the target position at that time, and released only when the cursor reaches that position (one each for the start / end of a drag-and-drop); if not in position after 1.5 seconds in the queue, report a failure and drop it.
// Called where the host reads the controller (hle_hid fill), reaching the game in the same sample as the keys
void dm_cursor_apply(uint64_t *buttons, int32_t *lx, int32_t *ly);
int dm_cursor_pending(void);   // 还有几下 A 扣着没送到游戏（光标没走到位）；前端收脉冲前要等它归零 | How many A presses are still held back and not delivered to the game (the cursor is not in position yet); the front end must wait for it to return to zero before ending the pulse
// 原生点击（界面鼠标消息注入，见 dm_cursor.c）：前端把触点交过来，宿主在游戏取手柄处逐帧发给界面根控件
// Native click (UI mouse message injection, see dm_cursor.c): the front end hands over the touch points, and the host sends them frame by frame to the UI root widget where the game reads the controller
void dm_ui_send_queue(const char *spec);   // 「类别:动作[:参数]」；下一次取手柄时往界面根控件发（按键脚本 MSG 步用） | 'category:action[:argument]'; sent to the UI root widget on the next controller read (used by the MSG step of key scripts)
int dm_ui_fe_drag_ok(void);                      // 前端此刻纵向拖能否当滚动（删除确认 / 模式页 / 起名页 / 确认框开着时不能） | Whether a vertical drag in the front end can count as a scroll right now (not while the delete confirmation / mode page / naming page / a confirmation box is open)
void dm_ui_touch(int phase, float nx, float ny);   // phase：0 按下 1 移动 2 抬起 3 作废（改成滚动）；坐标 = 出图画面归一化 [0,1] | phase: 0 down, 1 move, 2 up, 3 cancelled (turned into a scroll); coordinates = normalized to the rendered picture [0,1]
void dm_ui_mouse_pump(void);
// 通用指针事件：HOVER 开始悬停保持（每帧顶住光标）、MOVE 更新位置、END 结束保持；其余排队每帧一个。arg：SCROLL = 滚轮量（120 一格），KEY = VK 键码
// Generic pointer events: HOVER starts a held hover (pins the cursor every frame), MOVE updates the position, END ends the hold; the rest are queued, one per frame. arg: SCROLL = wheel amount (120 per notch), KEY = VK key code
enum { DM_UP_HOVER = 0, DM_UP_MOVE = 1, DM_UP_END = 2, DM_UP_LDOWN = 3, DM_UP_LUP = 4, DM_UP_RDOWN = 5, DM_UP_RUP = 6, DM_UP_SCROLL = 7, DM_UP_KEYDOWN = 8, DM_UP_KEYUP = 9 };
unsigned dm_ui_item_query(float nx, float ny);   // 问「这点是不是物品区」，返回问题序号；主线程下一帧作答 | Ask 'is this point an item area'; returns the question sequence number; the main thread answers on the next frame
int dm_ui_item_result(unsigned seq);             // 1 物品区 / 2 页签 / 0 面板内其它控件 / 3 面板外 / 6 压着被上层面板盖住的按钮（不点）/ 7 货架上的货 / 8 成叠格 / 10 单件槽占用时别的物品（不动）/ -1 还没答 | 1 item area / 2 tab / 0 other widget inside a panel / 3 outside the panels / 6 pressing on a button covered by an upper panel (not clicked) / 7 goods on the vendor shelf / 8 stacked cell / 10 another item while the single-item slot is occupied (left alone) / -1 not answered yet
unsigned dm_ui_drop_query(float nx, float ny_lifted, float ny_finger);   // 拖放落点怎么处理：结果 5 放在 dm_ui_drop_point 给的位置（物品区；装备槽取槽心）/ 3 丢弃 / 0 放回原处（同 dm_ui_item_result 取） | How to handle a drag-and-drop landing point: result 5 put at the position given by dm_ui_drop_point (item area; for an equipment slot the slot center) / 3 drop / 0 put back where it was (fetched with dm_ui_item_result as well)
int dm_ui_drop_at_now(float nx, float ny_lifted, float ny_finger);        // 同步版，只许游戏主线程调 | Synchronous version, callable only from the game main thread
int dm_ui_origin_point(float *nx, float *ny);   // 最近一次「拿起」的物品原位中心（放回原处用）；0 = 没有 | Center of the original position of the most recently 'picked up' item (used to put it back where it was); 0 = none
int dm_ui_drop_last(void);                      // 最近一次答上的拖放落点类（松手时这次的还没答上就用它；-1 = 没有） | Class of the most recently answered drag-and-drop landing point (used on release if this one has not been answered yet; -1 = none)
int dm_ui_drop_point(float *nx, float *ny);   // 落点判定为 5 时的放下位置（归一化） | Put-down position when the landing verdict is 5 (normalized)
int dm_ui_drop_bag(void);                       // 落点判定为 5 时落点是不是玩家背包格（货架拖买 / 成叠格拖出只在这里成交） | When the landing verdict is 5, whether the landing point is a player inventory cell (shelf drag-to-buy / dragging out of a stacked cell only complete here)
int dm_fe_bliss(void);   // 建角模式页的「极乐世界」勾没勾（1 = 勾了；玩家离开模式页后保留最后的值，建角时读）。任意线程可读 | Whether 「极乐世界」(Elysium) on the character-creation mode page is checked (1 = checked; the last value is kept after the player leaves the mode page and is read at character creation). Readable from any thread
int dm_ui_shelf_grid(float g[4]);   // 商人货架格网（归一化：左上角 x,y、一格宽、高）；0 = 还没探到 | Vendor shelf grid (normalized: top-left x,y, width and height of one cell); 0 = not probed yet
int dm_ui_item_rect(unsigned seq, float r[4]);   // 商人货架上被按住的那件货的屏幕矩形（归一化；拖货跟手图用）；0 = 没有 / 还没好，下一帧再问 | Screen rectangle of the goods being held down on the vendor shelf (normalized; used for the finger-following image when dragging goods); 0 = none / not ready yet, ask again next frame
int dm_ui_stack_left(void);                       // 这次按下的成叠格里那一叠有几件；-1 = 还没读到 | How many items are in the stack of the stacked cell pressed this time; -1 = not read yet
void dm_ui_world_tap(float ux, float uy, float wx, float wy);   // 游戏态点世界：点在佣兵头像上 = 开佣兵页，否则转给 dm_world_tap(wx, wy)。u = 视图归一化，w = 出图归一化 | Tap on the world in the game state: a tap on the mercenary portrait = open the mercenary page, otherwise forwarded to dm_world_tap(wx, wy). u = normalized to the view, w = normalized to the rendered picture
void dm_ui_stack_take_at(float nx, float ny);     // 高级仓库里的一叠拖到背包松手：光标放回那一叠并取 1 件到背包 | A stack in the advanced stash dragged to the inventory and released: put the cursor back on that stack and take 1 item into the inventory
void dm_ui_stack_cube_at(float nx, float ny);     // 高级仓库里的一叠拖到这一页下方的魔盒格上松手（落点类 11）：光标放回那一叠并按原版 X = 1 件进魔盒 | A stack in the advanced stash dragged onto the Horadric Cube cell at the bottom of this page and released (landing class 11): put the cursor back on that stack and press the original game's X = 1 item into the Horadric Cube
void dm_ui_shop_buy_at(float nx, float ny);       // 货架上的货拖到背包松手：把手柄光标放回那件货上（起拖时挪开了）并买下它 | Goods on the vendor shelf dragged to the inventory and released: put the controller cursor back on those goods (it was moved away when the drag started) and buy them
void dm_ui_item_tap(void);                       // 物品区单击：处在「用了鉴定卷轴 / 书、等选目标」的状态时 = 鉴定光标下那件；不在该状态什么都不做 | Single tap in an item area: when in the 'identify scroll / tome used, waiting for target selection' state = identify the item under the cursor; does nothing when not in that state
int dm_ui_targeting(void);                       // 此刻是否处在那个状态（宿主每帧发布，任意线程读） | Whether that state is in effect right now (published by the host every frame, read from any thread)
int dm_ui_panel_resident(const char *name);   // 局内常驻层（HUD 各层、佣兵头像栏、存盘指示、提示条…）：开着不算「有面板开着」 | In-run always-on layers (the HUD layers, mercenary portrait bar, save indicator, button legend...): being open does not count as 'a panel is open'
enum { DM_PS_BANK = 1, DM_PS_VENDOR = 2, DM_PS_DEAD = 4, DM_PS_TEXT = 8, DM_PS_IMBUE = 16, DM_PS_CUBE = 32, DM_PS_VAULT = 64 };   // VAULT = 储藏箱开在第六页「编年史」（dm_vault_ui.c）   // CUBE = 赫拉迪克方块页开着（HoradricCubeLayout）   // IMBUE = 单件任务槽面板开着（灌魔 / 打孔 / 个性化 / 古墓底座共用的 ImbueItemsPanel） | VAULT = the stash is open on the sixth page 「编年史」(Chronicle) (dm_vault_ui.c)   // CUBE = the Horadric Cube page is open (HoradricCubeLayout)   // IMBUE = the single-item task slot panel is open (ImbueItemsPanel, shared by imbue / socketing / personalize / tomb pedestal)
int dm_ui_panel_state(void);
void dm_ui_lifecycle(int background);   /* 前端进 / 出后台时调：位置守卫自检（dm_cursor.c mv_audit） | Called when the front end enters / leaves the background: position guard self-check (dm_cursor.c mv_audit) */   // 面板状态位（每帧读游戏面板树发布；变化时发合成界面消息 DM:PanelState） | Panel state bits (published by reading the game's panel tree every frame; on change the synthetic UI message DM:PanelState is sent)
int dm_ui_panel_shown(const char *name);   // 这个面板此刻在游戏面板树里可见（游戏主线程调） | This panel is visible in the game's panel tree right now (called from the game main thread)
int dm_ui_tap_box(void);   // 最近一次单击的物品是不是魔盒 | Whether the most recently single-tapped item is the Horadric Cube
uint32_t dm_ui_cur_stack_id(void);   // 仓库开着时光标此刻指着的若是背包里的物品 = 它的单位 id，否则 0 | While the stash is open, if the cursor is pointing at an item in the inventory right now = its unit id, otherwise 0
int dm_ui_free_cursor(void);   // 当前页有没有自由光标 | Whether the current page has a free cursor
void dm_ui_use_item(void);                       // 物品区双击：光标下是商人的货 → 买；能装备的 → 原版「按住确认键」那条（自动穿上）；其余 → 当场使用（喝药 / 读卷轴 / 开魔盒） | Double-tap in an item area: a vendor's goods under the cursor -> buy; equippable -> the original game's 'hold the confirm button' path (auto-equip); everything else -> use on the spot (drink a potion / read a scroll / open the Horadric Cube)
void dm_ui_long_press(void);                     // 物品区长按 = 补充到腰带：手柄光标指着的若是背包里能进腰带的药水且腰带有位，放进腰带；别的不动 | Long-press in an item area = refill the belt: if the controller cursor points at a potion in the inventory that can go into the belt and the belt has room, put it into the belt; anything else is left alone
void dm_ui_tab_go(void);                         // 最近一次命中是页签且这次是点按：按够次数的 ZL / ZR（顶部 L / R）切过去 | The most recent hit was a tab and this time it is a tap: press ZL / ZR (L / R at the top) enough times to switch to it
int dm_ui_item_at_now(float nx, float ny);        // 同上的同步版，只许游戏主线程调 | Synchronous version of the above; may only be called from the game main thread
void dm_ui_watch(const char *spec);            // 测试量具：盯住界面控件「名字[@面板名]」（NULL 取消）；主线程每帧发布它的中心 | Test probe: watch the UI widget 'name[@panel name]' (NULL cancels); the main thread publishes its center every frame
int dm_ui_watch_pos(float *nx, float *ny);    // 任意线程：盯住的控件可见返回 1 并给出游戏视图归一化坐标 | Any thread: returns 1 if the watched widget is visible and gives its coordinates normalized to the game view
uint64_t dm_ui_taps_base(void);   // 测试：DM_UI_TAPS_AT 命中时的出图帧（0 = 还没命中） | Test: presented frame at which DM_UI_TAPS_AT hit (0 = not hit yet)
void dm_ui_ptr(int kind, float nx, float ny, int arg);
int dm_ui_touch_active(void);
// 前端路径理顺（见 dm_cursor.c）：界面消息回调登记、取手柄处执行、自动按键掩码
// Front-end path streamlining (see dm_cursor.c): UI message callback registration, execution where the controller is read, auto key mask
void dm_ui_on_uimsg(const char *cat, const char *act, const char *arg);
void dm_ui_frontend_pump(void);
uint64_t dm_ui_auto_buttons(void);
void dm_ui_key_pulse(uint64_t mask, int frames);   // 替玩家点一下手柄键（按住 frames 个客体帧） | Tap a controller key on the player's behalf (held for frames guest frames)
// 界面连续手势（前端任意线程调，主线程每帧执行）：kind 1 = 纵向拖 → 滚动，2 = 横向拖 → 滑块；坐标 = 出图画面归一化 [0,1]；tol_ny = 认滑块时上下放宽多少（手指粗细）
// Continuous UI gestures (called by the front end from any thread, executed every frame on the main thread): kind 1 = vertical drag -> scroll, 2 = horizontal drag -> slider; coordinates = normalized to the rendered picture [0,1]; tol_ny = how much vertical slack to allow when identifying a slider (finger thickness)
void dm_ui_drag_begin(int kind, float ox, float oy, float tol_ny);
void dm_ui_drag_move(float x, float y);
void dm_ui_drag_end(void);
void dm_ui_drag_lift(float ny);   // 起拖前报物品图抬在手指上方多少（屏高比例）：编年史格子拖回背包按物品图位置判落点（dm_vault_ui.c） | Before a drag starts, report how far the item image is lifted above the finger (as a fraction of screen height): dragging a chronicle cell back to the inventory decides the landing point by the item image position (dm_vault_ui.c)
// 右上角小地图（游戏的 AutoMap 面板）在出图画面里的矩形，归一化 [0,1]：r = 左、上、右、下。返回 1 = 读到过（面板关着时给最近一次的）；任意线程调
// Rectangle of the top-right minimap (the game's AutoMap panel) in the rendered picture, normalized [0,1]: r = left, top, right, bottom. Returns 1 = has been read (the most recent one is given while the panel is closed); callable from any thread
int dm_ui_minimap_rect(float r[4]);
// 全屏地图 ↔ 小地图（运行时切，不落盘）：dm_ui_map_full(1) 切全屏、(0) 回玩家设置的那档小地图；dm_ui_map_is_full() = 游戏当前是不是全屏地图（任意线程调，主线程每帧刷新）
// 技能绑定的触摸流程（前端任意线程调，主线程每帧推进）：dm_ui_bind_begin(键) = 给这个手柄键（可带 ZL = 第二组）选技能——宿主打开技能树，
// 玩家点一个技能（前端在流程进行中把点按改成「光标到点 + 按 Y」）→ 弹出原版「重新绑定技能」框 → 宿主替他按目标键 → 关面板。dm_ui_bind_active() = 流程进行中（0 无 / 1 正在开技能树 / 2 等玩家点技能 / 3 收尾）
// Full-screen map ↔ minimap (switched at runtime, not written to disk): dm_ui_map_full(1) switches to full screen, (0) returns to the minimap level set by the player; dm_ui_map_is_full() = whether the game is currently on the full-screen map (callable from any thread, refreshed every frame on the main thread)
// Touch flow for skill binding (called by the front end from any thread, advanced every frame on the main thread): dm_ui_bind_begin(key) = choose a skill for this controller key (may carry ZL = second set) -- the host opens the skill tree,
// the player taps a skill (while the flow is in progress the front end turns the tap into 'cursor to the point + press Y') -> the original game's 「重新绑定技能」(Press button to rebind skill) box pops up -> the host presses the target key for the player -> the panel closes. dm_ui_bind_active() = flow in progress (0 none / 1 opening the skill tree / 2 waiting for the player to tap a skill / 3 wrapping up)
void dm_ui_bind_begin(uint64_t key_mask);
int dm_ui_bind_slot(void);   /* 绑定目标位 1..8（F1–F8），不在绑定态 0 | Binding target slot 1..8 (F1–F8), 0 when not in the binding state */
void dm_ui_safe_left(int units);    /* 左安全区，布局单位（2160 高）；左上角佣兵 / 队友头像栏按它内移 | Left safe area, in layout units (2160 high); the mercenary / party member portrait bar in the top-left corner is moved inward by it */
void dm_ui_safe_right(int units);   /* 右安全区，布局单位（2160 高）；右上角小地图与信息栏按它内移 | Right safe area, in layout units (2160 high); the minimap and info bar in the top-right corner are moved inward by it */
uint32_t dm_ui_hit_id(void);   /* 最近一次命中的加点类按钮：属性「+」1..4（力量 / 敏捷 / 体力 / 精力），技能图标 = 最高位置 1 的标识；没命中 0 | The most recently hit point-spending button: attribute '+' 1..4 (strength / dexterity / vitality / energy), skill icon = an identifier with the top bit set to 1; 0 if nothing was hit */
int dm_ui_bind_active(void); uint64_t dm_ui_auto_release(void);   /* 取手柄处用：此刻要从玩家输入里松开的键 | Used where the controller is read: the keys to release from the player's input right now */ int dm_ui_bind_skill_hit(float *nx, float *ny);   /* 最近一次命中（吸附后）落在加点类按钮上：1 = 技能树的技能图标，2 = 属性页的「+」；带归一化坐标 | The most recent hit (after snapping) landed on a point-spending button: 1 = a skill icon in the skill tree, 2 = a '+' on the attributes page; comes with normalized coordinates */
void dm_ui_bind_cancel(void);
void dm_ui_map_full(int on);
int dm_ui_map_is_full(void);
void dm_ui_text_entered(const char *text);   // 系统键盘返回时调：text = 写回给游戏的最终文本（UTF-8）；取消 / 空串传 NULL | Called when the system keyboard returns: text = final text written back to the game (UTF-8); pass NULL for cancel / empty string
// 延迟读数（诊断，DM_LAT_LOG=1）：关键时刻打一行「LAT <毫秒时刻> <事件>」，量手指落下 / 松手到游戏生效的延迟
// Latency readout (diagnostic, DM_LAT_LOG=1): at key moments print a line 「LAT <毫秒时刻> <事件>」(LAT <millisecond timestamp> <event>), to measure the latency from finger down / release to the game taking effect
int dm_lat_on(void);
#define DM_LAT(fmt, ...) do { if (dm_lat_on()) LOG("LAT %llu " fmt, (unsigned long long)(now_ns() / 1000000), ##__VA_ARGS__); } while (0)
void dm_ui_hand_idle(int idle);                  // 触摸前端每拍报：没有手指、没有在发 / 排着的键与光标请求 = 1（手上物品守门用，dm_cursor.c hand_guard） | Reported by the touch front end every tick: no fingers, no keys or cursor requests being sent / queued = 1 (used by the held-item guard, dm_cursor.c hand_guard)
uint32_t dm_ui_hover_unid(void);               // 光标此刻指着的我方未鉴定物品 id（背包 / 箱子 / 方块 / 身上；没有 = 0）。宿主每帧发布，任意线程读 | id of our own unidentified item the cursor is pointing at right now (inventory / stash / cube / equipped; none = 0). Published by the host every frame, read from any thread
void dm_ui_identify_hovered(void);              // 触摸层双击未鉴定物品：宿主下一帧对它调免费鉴定（dm_item_identify_free） | Touch layer double-tap on an unidentified item: on the next frame the host calls free identify on it (dm_item_identify_free)
int dm_ui_map_geom(float *cell);   // 地图几何：返回 0 没开 / 1 微缩 / 2 全屏，*cell = 每格像素（真实画面像素，任意线程） | Map geometry: returns 0 not open / 1 minimap / 2 full screen, *cell = pixels per cell (real picture pixels, any thread)
