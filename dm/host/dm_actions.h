// 「先选中、再确认」的宿主入口：商人买卖、属性 / 技能加点、数量框设值。实现与依据见 dm_actions.c、dm_textinput.c。
// 动作：任意线程调，游戏主线程（取手柄处）执行。读数：主线程每帧发布快照，任意线程读。
// Host entry points for "select first, then confirm": vendor buying/selling, stat / skill point allocation, setting the quantity box value. For implementation and rationale see dm_actions.c, dm_textinput.c.
// Actions: callable from any thread, executed on the game main thread (where the controller is fetched). Readings: the main thread publishes a snapshot every frame, readable from any thread.
#pragma once
#include <stdint.h>
typedef struct {
    int open;                 // 商人交易页开着 | the vendor trade page is open
    int gold, stash_gold;     // 身上金币、箱内金币（买东西两处都能扣，游戏规则） | gold carried, gold in the stash (purchases can deduct from both, game rule)
    int sel_valid;            // 光标选中了一件货 | the cursor has selected a piece of goods
    uint32_t sel_id; char sel_code[4]; char sel_name[64]; int buy_price;
    int sel_gx, sel_gy, sel_gw, sel_gh;   // 选中那件货在货架上的左上格与占格（格数；sel_valid 为 0 时无意义）。换成屏幕矩形给拖货跟手图用 | top-left cell and footprint of the selected goods on the shelf (in cells; meaningless when sel_valid is 0). Converted to a screen rectangle for the drag-goods follow image
    int can_buy;              // 钱够（买价 ≤ 身上 + 箱内金币）；背包放不放得下由游戏在购买时判 | enough gold (buy price ≤ gold carried + gold in the stash); whether the inventory has room is judged by the game at purchase time
    int held_valid;           // 手上拿着物品（拖着） | an item is held in hand (being dragged)
    uint32_t held_id; char held_name[64]; int sell_price;   // sell_price = 0：这件不能卖 | sell_price = 0: this item cannot be sold
} dm_shop_info;
void dm_shop_get(dm_shop_info *out);
int  dm_shop_open(void);
void dm_shop_buy(void);                   // 买选中的那件（游戏自己的购买路径；钱不够 / 背包放不下由游戏拒并弹原版提示，这里不拦） | buy the selected item (the game's own purchase path; not enough gold / no room in the inventory is rejected by the game with the original game's prompt, not blocked here)
void dm_shop_sell_cursor(void);           // 卖手上拿着的那件 | sell the item held in hand

typedef struct {
    int stat_points, skill_points;        // 剩余属性点 / 技能点 | remaining stat points / skill points
    int tree_open;                        // 技能树页开着 | the skill tree page is open
    int char_open;                        // 角色页开着（且高级属性页没盖在上面）；手机层据此画属性「确认 +1 / +5 / 全部」 | the character page is open (and the advanced stats page is not covering it); the phone layer draws the stat "confirm +1 / +5 / all" based on this
    int sel_skill;                        // 技能树页当前选中的技能号，-1 = 没有 | skill id currently selected on the skill tree page, -1 = none
    int sel_can_add;                      // 还能不能加（= sel_block == 0） | whether a point can still be added (= sel_block == 0)
    int sel_block;                        // 不能加的原因：0 可加，1 没有技能点，2 等级不够，3 前置技能没学，4 其它（已到上限等） | reason a point cannot be added: 0 can add, 1 no skill points, 2 level too low, 3 prerequisite skill not learned, 4 other (already at the cap, etc.)
    char sel_name[64];
} dm_points_info;
void dm_points_get(dm_points_info *out);
void dm_stat_add(int stat, int n);        // stat：0 力量 1 敏捷 2 体力 3 精力；n：1 / 5 / -1 全部（角色页开着时有效） | stat: 0 strength 1 dexterity 2 vitality 3 energy; n: 1 / 5 / -1 all (valid while the character page is open)
void dm_skill_add_selected(void);         // 给技能树页当前选中的技能加 1 点 | add 1 point to the skill currently selected on the skill tree page
void dm_belt_put(uint32_t item_id);        // 把背包里的这件物品放进腰带（游戏自己的「移到腰带」路径；不是药水 / 腰带满时不动）。item_id 用 dm_cursor_item 的 id | put this inventory item into the belt (the game's own "move to belt" path; does nothing if it is not a potion / the belt is full). item_id uses the id from dm_cursor_item
void dm_item_use(uint32_t item_id);         // 立即使用这件物品（原版「使用」同一个函数：手柄按住确认键 / 键鼠右键的终点；不等长按门槛）。喝药、读卷轴、用书、开方块。item_id 用 dm_cursor_item 的 id | use this item immediately (the same function as the original game's "use": the endpoint of controller hold-confirm / keyboard-and-mouse right click; does not wait for the long-press threshold). Drinks potions, reads scrolls, uses tomes, opens the cube. item_id uses the id from dm_cursor_item
int  dm_ui_hover_now(int x, int y);        // 把原版手柄光标放到 (x, y)（与 dm_cursor.c 写的是同一对坐标）并当场让游戏重算悬停 / 焦点 / 选中格，返回 1 = 已刷新、0 = 此刻不是手柄光标模式。只能在游戏主线程（取手柄处）调 | put the original game's controller cursor at (x, y) (the same coordinate pair dm_cursor.c writes) and have the game recompute hover / focus / selected cell on the spot; returns 1 = refreshed, 0 = not in controller cursor mode right now. May only be called on the game main thread (where the controller is fetched)
void dm_item_compare(int on);              // 装备对比开 / 关：开着时，悬停 / 选中的装备说明框旁并排出身上同部位那件（原版「按住比较键」的同一个状态位，不经手柄键， | equipment comparison on / off: while on, the item worn in the same slot is shown side by side next to the hovered / selected equipment's tooltip (the same state bit as the original game's "hold the compare button", not going through the controller button,
                                           // 所以商人页、储藏箱页里比较键被翻页签占用也能用）。任意线程调；开着期间宿主每帧保持。赌博页原版不支持对比，开了也不出
                                           // so it also works on the vendor page and the stash page, where the compare button is taken up by tab switching). Callable from any thread; while on, the host maintains it every frame. The original game does not support comparison on the gambling page, so nothing shows there even when on
void dm_belt_expand(int on);               // 展开 / 收起腰带上面几排（原版键鼠「显示腰带」动作开关的同一个面板，面板号 0x1a）。任意线程调 | expand / collapse the upper rows of the belt (the same panel toggled by the original game's keyboard-and-mouse "show belt" action, panel id 0x1a). Callable from any thread
void dm_belt_use(int col, int row);        // 喝腰带里第 row 排（0 = 底排）第 col 列（0–3）的那一瓶；那一格是空的就不动。走 dm_item_use 的腰带分支。任意线程调 | drink the bottle in row `row` (0 = bottom row), column `col` (0–3) of the belt; does nothing if that cell is empty. Goes through the belt branch of dm_item_use. Callable from any thread
void dm_belt_use_merc(int col, int row);   // 同上，但喂给雇佣兵（腰带双击）；雇佣兵不在 / 已死 / 这瓶不能喂就什么都不做 | same as above, but feeds it to the mercenary (belt double-tap); does nothing if the mercenary is absent / dead / this bottle cannot be fed
int  dm_belt_rows(int *expanded);          // 腰带排数（1–4，取决于身上的腰带）；expanded 非空时写 1 = 现在展开着。读的是主线程每帧发布的快照 | belt row count (1–4, depending on the belt worn); when expanded is non-null, writes 1 = currently expanded. Reads the snapshot the main thread publishes every frame
void dm_held_equip(int bodyloc);          // 把手上拿着的物品放到身上这个栏位（原版装备槽「手上有物品时按确认键」的同一个函数 main+0x3bb24c；空槽 = 装上，有物品 = 交换；原版拒绝的不动）。任意线程调 | put the held item into this body slot (the same function as the original game's equipment slot "press confirm while holding an item", main+0x3bb24c; empty slot = equip, occupied = swap; nothing happens for what the original game rejects). Callable from any thread
void dm_inv_sort(int page);                // 整理（自动排列）：page 0 背包、3 赫拉迪克方块、4 个人储藏箱当前页。原版「按住排序」的终点函数，不等长按。任意线程调 | sort (auto-arrange): page 0 inventory, 3 Horadric Cube, 4 current page of the personal stash. The endpoint function of the original game's "hold to sort", without waiting for the long press. Callable from any thread
void dm_name_random(void);                 // 建角起名页：给名字框填一个随机名（原版「随机名字」键的同一条消息）。名字面板开着时有效。任意线程调 | character creation naming page: fill the name box with a random name (the same message as the original game's 「随机名字」(Random Name) button). Valid while the name panel is open. Callable from any thread
int dm_name_taken(const char *name);       // 已有这个名字的角色（存档工作目录下有同名目录）返回 1 | returns 1 if a character with this name already exists (a same-named directory exists under the save working directory)
int dm_name_gen(char *out, int cap, int (*taken)(const char *));   // 起名页留空时用的随机名（dm_namegen.c）：音节拼接，taken 非 0 就再抽，至多 64 次；成功返回 1 | random name used when the naming page is left blank (dm_namegen.c): syllable concatenation, draws again if taken is non-zero, at most 64 times; returns 1 on success
void dm_item_equip(uint32_t item_id);       // 立即装备背包（或箱子 / 方块）里的这件到对应栏位（原版「按住确认键 = 装备」的终点；栏位由游戏选，已有装备时游戏自己交换；雇佣兵面板开着且他能穿时给雇佣兵，同原版）。 | immediately equip this item from the inventory (or stash / cube) into the corresponding slot (the endpoint of the original game's "hold confirm = equip"; the slot is chosen by the game, and the game swaps by itself when something is already equipped; when the mercenary panel is open and he can wear it, it goes to the mercenary, same as the original game).
                                           // 穿不了（等级 / 属性不够、未鉴定、不是装备）时只播原版的错误音，不发包
                                           // When it cannot be worn (level / stats too low, unidentified, not equipment) only the original game's error sound is played and no packet is sent
void dm_item_unequip(uint32_t item_id);     // 立即把身上穿着的这件卸到背包（原版装备栏「按住确认键 = 卸下」的终点；背包放不下时游戏播错误音、不动）。商人页开着时不执行（原版那条路在商人页里会变成卖掉） | immediately unequip this worn item into the inventory (the endpoint of the original game's equipment panel "hold confirm = unequip"; when the inventory cannot hold it the game plays the error sound and does nothing). Not executed while the vendor page is open (in the vendor page that original path would turn into selling it)
void dm_item_identify(uint32_t target_id);   // 「用了鉴定卷轴 / 鉴定书、等待选目标」状态下（dm_item_targeting() 非 0），对这件物品完成使用（原版物品格 / 装备栏点击处理在该状态下的终点：发「对物品使用物品」包；卷轴消耗 / 书扣次数由服务端照原版做）。 | in the "used an identify scroll / identify tome, waiting to pick a target" state (dm_item_targeting() non-zero), complete the use on this item (the endpoint of the original game's item cell / equipment panel click handling in that state: sends the "use item on item" packet; scroll consumption / tome charge deduction is done by the server as in the original game).
                                             // 不在该状态、目标不是自己身上 / 背包 / 箱子 / 方块里的物品、目标有待确认操作时不调（日志写原因）
                                             // Not called when not in that state, when the target is not an item on oneself / in the inventory / stash / cube, or when the target has an operation pending confirmation (the log writes the reason)
void dm_item_repair(uint32_t item_id);        // 商人页修理态下修这一件（原版修理态点物品的同一个终点）；不在修理态 / 手上有东西 / 原版判定不用修时不调（日志写原因） | repair this one item in repair mode on the vendor page (the same endpoint as clicking an item in the original game's repair mode); not called when not in repair mode / something is in hand / the original game judges no repair is needed (the log writes the reason)
void dm_belt_fill(void);                     // 物品栏里的腰带：背包里能进腰带的药一次装满空位（原版手柄「焦点在腰带上按住 R」的同一段）；条件不满足不动。任意线程可调 | Belt in the inventory: fills its empty slots at once with belt-eligible potions from the backpack (the same logic as the original controller "hold R with focus on the belt"); does nothing if conditions are not met. Callable from any thread
void dm_item_target_cancel(void);            // 退出「等待选目标」状态，不耗卷轴（原版点到空处 / 无效目标时做的同一段：清待用物品、光标状态复位）。不在该状态时什么都不做 | leave the "waiting to pick a target" state without consuming the scroll (the same code the original game runs when clicking on empty space / an invalid target: clears the pending item, resets the cursor state). Does nothing when not in that state
void dm_stash_take(int count);                   // 高级仓库（宝石 / 材料 / 符文页）里光标此刻指着的那一叠，取 count 件到背包（命令 0x63、去向 = 背包；1 件走格控件方法 main+0x39d824，多件走 main+0x39d9e0 带件数）。 | from the stack the cursor is pointing at right now in the advanced stash (gems / materials / runes pages), take count pieces into the inventory (command 0x63, destination = inventory; 1 piece goes through the grid widget method main+0x39d824, multiple pieces go through main+0x39d9e0 with a piece count).
                                                 // count 超过叠数时取剩下的全部。光标下没有物品 / 不是成叠物品 / 箱子没开时不动，日志「取多件：」写原因。背包放不下时由服务端决定取几件（没验）
                                                 // When count exceeds the stack count, takes all that remain. Does nothing when there is no item under the cursor / it is not a stacked item / the stash is not open; the log 「取多件：」(take multiple:) writes the reason. When the inventory cannot hold them, the server decides how many to take (not verified)
void dm_stash_take_id(uint32_t item_id, int count);   // 同上，按物品 id 指定（测试用） | same as above, specified by item id (for testing)
void dm_num_set(int n);                   // 数量框设值：≥0 该数，-1 全部，-2 一半（dm_textinput.c） | set the quantity box value: ≥0 that number, -1 all, -2 half (dm_textinput.c)
void dm_actions_pump(void);               // 取手柄处每帧调（主线程） | called every frame where the controller is fetched (main thread)
void dm_held_stow(void);                     // 把手上（光标上）那件物品收进背包、不关面板：原版「手上有物品时按返回」做的那一步（main+0x3cf4d0：给物品在背包里找位置并发 0x18）。任意线程调，下一帧在游戏主线程执行 | put the item in hand (on the cursor) into the inventory without closing the panel: the step the original game does for "press back while holding an item" (main+0x3cf4d0: finds a place in the inventory for the item and sends 0x18). Callable from any thread, executed on the game main thread next frame
int dm_held_stow_result(void);               // 最近一次 dm_held_stow 的结果：-1 还没出结果（没放成会逐帧重试约 1.5 秒：刚交换到手上的物品要等服务器回执）；1 已放进背包（已发包）；0 没放成，手上的物品不动 | result of the most recent dm_held_stow: -1 no result yet (if it was not stowed it retries every frame for about 1.5 seconds: an item just swapped into the hand has to wait for the server's acknowledgement); 1 put into the inventory (packet sent); 0 not stowed, the held item is untouched
void dm_item_identify_free(uint32_t item_id);   // 免费鉴定这件（未鉴定物品双击）：服务端拍里调原版鉴定函数 main+0x78acf0，不消耗卷轴 / 书 | identify this item for free (double-tap on an unidentified item): calls the original game's identify function main+0x78acf0 in the server tick, without consuming a scroll / tome
// 副手组技能一键放（dm_actions.c 那一节）
// One-press casting of off-hand set skills (that section of dm_actions.c)
uint64_t dm_swapskill_filter(uint64_t buttons);   // 取手柄状态处（hle_hid.c）：压住 / 放行要先换武器的那个键 | where the controller state is fetched (hle_hid.c): hold back / let through the button that needs a weapon swap first
void dm_swapskill_srv_wset(int w);                // 服务端拍（diag.c hud_tick）发布当前武器组 0 / 1 | the server tick (diag.c hud_tick) publishes the current weapon set 0 / 1
int dm_swapskill_slot(int slot, int *charges);    // 技能钮显示：1 = 技能在另一组（「II」角标），2 = 充能技能（*charges = 剩余次数） | skill button display: 1 = the skill is in the other set ("II" badge), 2 = charged skill (*charges = remaining charges)
int dm_swapskill_equipped(int32_t item_id);       // 这件物品身上穿着（含换手那组） | this item is worn on the body (including the weapon-swap set)
int dm_swapskill_offhand_gives(int skill);         // 换手那组的装备给这个非职业技能（战争召唤等，绑定里物品 -1） | the gear in the weapon-swap set grants this non-class skill (Call to Arms and the like; item -1 in the binding)
