// 点游戏世界里的目标 = 与它交互（点物品名 = 拾取；除摇杆外触点不触发点地移动，但由目标触发的走近保留）。
// 发令复用游戏自己的点击入口 main+0x4ace20(玩家, 动作, x, y, 标志)——手柄主键（0x2b5c38 动作 7 按下 / 0x2b5ae4 动作 2 松开）与键鼠左键共用，
// 它自己处理「离得远先走过去、到了再交互」（挂起交互）和各类目标的命令包（物品 0x16、NPC 0x2f、物件 0x41、出口 0x40）。
// 目标经输入管理器（main+0x2acecc() 返回）的「指定目标」槽 [+0xd10] 交给它（0x4acec4：非空即用）。地址为 3.3.94012 原映像偏移。只许游戏主线程调。
// Tapping a target in the game world = interacting with it (tapping an item name = pick up; apart from the stick, touch points do not trigger click-ground movement, but the approach triggered by a target is kept).
// The command reuses the game's own click entry main+0x4ace20(player, action, x, y, flags) -- shared by the controller primary button (0x2b5c38 action 7 press / 0x2b5ae4 action 2 release) and the keyboard-and-mouse left button,
// which itself handles "walk over first when far away, interact on arrival" (pending interaction) and the command packets for each kind of target (item 0x16, NPC 0x2f, object 0x41, exit 0x40).
// The target is handed to it through the "designated target" slot [+0xd10] of the input manager (returned by main+0x2acecc()) (0x4acec4: used whenever non-null). Addresses are offsets into the original 3.3.94012 image. May only be called from the game main thread.
#include "touch_mac.h"
#include "dm_uiscreen.h"
#include "dm_world.h"
#include "hle.h"
#include "dm.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>
#include "dm_unit.h"
#include "dm_hud.h"
#include "dm_prof.h"
#include <TargetConditionals.h>

static uint8_t *wb(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static uint64_t w_player(void) { uint8_t *b = wb(); return b ? ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)) : 0; }
static int w_unit_xy(uint64_t u, int *x, int *y) {   // 单位格坐标：物件 / 物品 / 出口砖是路径 +0x10 / +0x14，玩家 / 怪物是路径头的 16 位 x / y（同 0x4acf54–0x4ad038） | unit cell coordinates: for objects / items / exit tiles path +0x10 / +0x14, for players / monsters the 16-bit x / y in the path header (same as 0x4acf54–0x4ad038)
    uint64_t p = *(uint64_t *)(u + 0x38); if (!p) return 0; uint32_t t = *(uint32_t *)u;
    if (t == 2 || t == 4 || t == 5) { *x = *(int32_t *)(p + 0x10); *y = *(int32_t *)(p + 0x14); } else { *x = *(uint16_t *)(p + 2); *y = *(uint16_t *)(p + 6); }
    return 1;
}
_Atomic uint64_t g_dm_move_allow_until_ns;   // 「点地移动包一律丢弃」（diag.c c2s_deliver）的豁免窗：由目标触发的走近 | exemption window for "click-ground move packets are always dropped" (diag.c c2s_deliver): an approach triggered by a target
static uint64_t w_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t g_release_at; static int g_rel_x, g_rel_y;
// 来自 hubeiqiao 的 （公开仓库 诊断）
// 物件交互的只读样本：不把返回 1 当换图成功，也不补发点击。新交互会明确结束旧追踪。
// From hubeiqiao (public repo diagnostics)
// Read-only samples of object interaction: a return of 1 is not treated as a successful level change, and no click is re-sent. A new interaction explicitly ends the old trace.
static struct { uint64_t seq, frame; uint32_t player; int active, stage, last_lv; } g_it;
static uint64_t w_interaction_begin(uint64_t pl, uint64_t u, uint64_t fr) {
    if (*(uint32_t *)(u) != 2) return 0;
    if (g_it.active) LOG("物件交互诊断：序号 %llu 被新交互替代", (unsigned long long)g_it.seq);
    g_it.seq++; g_it.frame = fr; g_it.player = *(uint32_t *)(pl + 8); g_it.active = 1; g_it.stage = 0; g_it.last_lv = dm_unit_level(pl);
    int x = 0, y = 0; w_unit_xy(u, &x, &y);
    LOG("物件交互诊断：序号 %llu 帧 %llu 玩家 %u 客户端关卡 %d 目标类号 %u id %u 坐标 %d,%d", (unsigned long long)g_it.seq, (unsigned long long)fr, g_it.player, g_it.last_lv, *(uint32_t *)(u + 4), *(uint32_t *)(u + 8), x, y);
    return g_it.seq;
}
static void w_interaction_poll(uint64_t pl, uint64_t fr) {
    if (!g_it.active) return;
    if (!pl || *(uint32_t *)(pl + 8) != g_it.player || fr < g_it.frame) { LOG("物件交互诊断：序号 %llu 玩家离局或切换，结束", (unsigned long long)g_it.seq); g_it.active = 0; return; }
    int lv = dm_unit_level(pl); uint64_t age = fr - g_it.frame;
    static const uint64_t at[] = { 3, 30, 120 };
    if (lv == g_it.last_lv && (g_it.stage >= 3 || age < at[g_it.stage])) return;
    while (g_it.stage < 3 && age >= at[g_it.stage]) g_it.stage++;
    int x = 0, y = 0; w_unit_xy(pl, &x, &y); dm_hud_info hi; dm_hud_get(&hi);
    LOG("物件交互诊断：序号 %llu 后 %llu 帧 客户端关卡 %d 坐标 %d,%d 模式 %u 服务端最近快照 %d 有效 %d（样本非换图成功判据）", (unsigned long long)g_it.seq, (unsigned long long)age, lv, x, y, *(uint32_t *)(pl + 0xc), hi.area, hi.valid);
    g_it.last_lv = lv; if (age >= 120) g_it.active = 0;
}
static int w_interact(uint64_t u) {   // 就近交互：游戏的点击入口（按下，3 帧后松开） | interact nearby: the game's click entry (press, release 3 frames later)
    uint8_t *b = wb(); uint64_t pl = w_player(); int x = 0, y = 0; if (!b || !pl || !u || !w_unit_xy(u, &x, &y)) return 0;
    uint64_t mgr = ((uint64_t (*)(void))(b + 0x2acecc))(), keep = *(uint64_t *)(mgr + 0xd10);
    atomic_store(&g_dm_move_allow_until_ns, w_now_ns() + 500000000ull);
    uint64_t trace = w_interaction_begin(pl, u, dm_guest_frame());
    *(uint64_t *)(mgr + 0xd10) = u;
    int r = ((int (*)(uint64_t, int, int, int, uint64_t))(b + 0x4ace20))(pl, 7, x, y, 0);
    *(uint64_t *)(mgr + 0xd10) = keep;
    if (trace) LOG("物件交互诊断：序号 %llu 点击入口返回 %d（尚未确认换图）", (unsigned long long)trace, r);
    g_release_at = dm_guest_frame() + 3; g_rel_x = x; g_rel_y = y;
    return r;
}
// 走近再交互。游戏点击入口自带的走近是手柄式的「朝目标直线走几步」（被箱子之类挡住就停，实测走向 35 格外的 NPC 停在 7 格处），
// 远处目标改由这里发「跑到目标所在格」（main+0x4ad870(3, 玩家, x, y) → 命令包 0x03，服务端寻路），进到 W_NEAR 格内再走游戏的点击入口。
// 取消：摇杆动了 / 目标没了 / 15 秒没到（营地里最远的 NPC 要跑 9 秒左右）/ 又点了别的。NPC 会走动：每 20 帧按它的当前位置重发一次。
// Approach, then interact. The approach built into the game's click entry is the controller-style "walk a few steps straight toward the target" (it stops when blocked by a chest or the like; measured walking to an NPC 35 cells away, it stopped at 7 cells),
// so for distant targets this code sends "run to the target's cell" instead (main+0x4ad870(3, player, x, y) -> command packet 0x03, server-side pathfinding), and goes through the game's click entry once within W_NEAR cells.
// Cancel: stick moved / target gone / not arrived in 15 seconds (the farthest NPC in the camp takes about 9 seconds to run to) / something else tapped. NPCs walk: resend every 20 frames with its current position.
#define W_NEAR 2   // 走近的停点：落在原生主钮（手柄 A）的交互门槛之内。门槛是游戏自己的「当前交互目标」（main+0x2f7a0c）存不存在，实测：箱子差 3 格出现、4 格没有； | stop point of the approach: falls inside the interaction threshold of the native primary button (controller A). The threshold is whether the game's own "current interaction target" (main+0x2f7a0c) exists, measured: a chest appears at 3 cells away and not at 4;
                   // 小站（占地大）差 (3,4) 出现；NPC 在 4 格处时有时无（他在走、人停下后又没了）。停在 4 格时点目标能交互（我们带着目标调点击入口），但关掉对话再按主钮就按不出来
                   //（阿卡拉 4 格 0/5、2 格 5/5）。所以一律走到 2 格内再交互；走不进 2 格（目标自己占着格子、被挡）而已在 W_FAR 格内时就地交互
                   // A waypoint (large footprint) appears at offset (3,4); an NPC at 4 cells comes and goes (he is walking, and it is gone again after the player stops). Stopped at 4 cells, tapping the target can interact (we call the click entry with the target), but after closing the dialog pressing the primary button does nothing
                   // (Akara 4 cells 0/5, 2 cells 5/5). So always walk to within 2 cells before interacting; when 2 cells cannot be reached (the target itself occupies the cell, or it is blocked) and the player is already within W_FAR cells, interact in place
#define W_FAR 4
#if TARGET_OS_OSX
#define W_GROUND 99   // g_go.type 的特殊值：目标不是单位，是地面上的一格（Mac 鼠标点空地走过去；只有 macOS 宿主有） | special value of g_go.type: the target is not a unit but a tile on the ground (Mac mouse click-to-walk; macOS host only)
#define W_NEAREST_OK(t) ((t) == 2 || (t) == W_GROUND)
static int g_hit_mon;   // 这一次命中把敌对怪也算进来（Mac 鼠标点怪 = 攻击；触屏不算，攻击归技能键） | this hit test also counts hostile monsters (Mac mouse click on a monster = attack; touch does not, attacking belongs to the skill buttons)
#define W_MON_OK(cat) (g_hit_mon && ((cat) == 1 || (cat) == 0xa))   // 交互类别 1 = 普通怪、0xa = 另一类可攻击的（同下面测试开关 monster 的认法） | interaction category 1 = ordinary monster, 0xa = another attackable kind (the same recognition as the monster test switch below)
static int g_gnd_x, g_gnd_y;   // 地面目标格 | ground target tile
static uint64_t g_gnd_stick_seen, g_gnd_stick_at;   // 摇杆时间戳上次读到的值、它变化的时刻（本文件的时钟；那个时间戳是别的时钟记的，只能比变没变） | the last value read of the stick timestamp and the moment it changed (this file's clock; that timestamp is recorded by another clock, so only whether it changed can be compared)
#else
#define W_NEAREST_OK(t) ((t) == 2)
#define W_MON_OK(cat) 0
#endif
static struct { uint32_t type, id; uint64_t until_ns, stick, next_fr, still_fr; int on, lx, ly; int n, i, tx, ty, replans, stop_at, empty, direct, alt, alt_on, ax, ay, nearest; uint16_t node[24][2]; } g_go;   // stop_at > 0：只走到这么多格内就停、不交互（测试开关「走到怪旁」用） | stop_at > 0: only walk to within this many cells and stop, without interacting (used by the test switch "walk next to a monster")
static uint64_t w_find(uint32_t type, uint32_t id) { uint8_t *b = wb(); return type <= 5 ? ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + type * 0x400), id & 0x7f, id, type) : 0; }
static void w_go(uint64_t u) { uint64_t dm_stick_last_ns(void); g_go.type = *(uint32_t *)u; g_go.nearest = 0; g_go.id = *(uint32_t *)(u + 8); g_go.until_ns = w_now_ns() + 15000000000ull; g_go.stick = dm_stick_last_ns(); g_go.next_fr = 0; g_go.still_fr = 0; g_go.n = g_go.i = g_go.replans = g_go.empty = g_go.direct = g_go.alt = g_go.alt_on = 0; g_go.stop_at = 0; g_go.on = 1; }
// 路线规划：宿主自己在游戏的碰撞格上做最短路（广度优先），再拉直成几段直线，逐段发游戏自己的「跑到点」（0x4ad870 + 服务端移动）。只换规划，不换游戏逻辑。
// 为什么不用游戏的寻路函数（main+0x59fa20）当规划器（三条原因）：①它是「朝目标走、贴着障碍滑」的贪心，会把人带进死角（哈洛加斯走向安亚：停在建筑北墙 22 格外，再算返回 0）；
// ②它不把别的单位算障碍，而执行「跑到点」时单位会挡（紧挨瓦瑞夫出发：第一段穿过他脚下，人一步不动，重算同解）；③单段太长游戏不走（56 格一段包发了人不动）。
// 为什么不直接发「对单位交互」让原版走：玩家缺省的路径类型 7 是碰到障碍就停，隔着墙 / 箱子点 NPC 会停在障碍前。
// 碰撞格只读：房间 {+0x80 x, +0x84 y, +0x88 宽, +0x8c 高（格）, +0x38 → {+8 宽, +0x20 u16 数组}, +0 相邻房间数组, +0x40 个数}（同游戏的连线检查 main+0x4d1fc0 读的那份）。
// 跑在游戏主线程取手柄处（dm_world_pump）；数组全是静态的，不占客体线程栈。耗时（Mac 宿主实测）：城里常见 0.02–0.6 ms，目标到不了的最坏情况 0.55–1.13 ms（搜 7 千–1.9 万格）；
// 展开上限 WP_CAP 格封顶。到不了时只算这一次就取消，不连着算；原地不动 1 秒才重算一次，最多 3 次。
// Route planning: the host itself runs a shortest path (breadth-first) on the game's collision grid, straightens it into a few straight segments, and sends the game's own "run to point" segment by segment (0x4ad870 + server-side movement). Only the planning is replaced, not the game logic.
// Why the game's pathfinding function (main+0x59fa20) is not used as the planner (three reasons): (1) it is a greedy "walk toward the target, slide along obstacles" and leads the player into dead ends (Harrogath walking to Anya: stops 22 cells away at the north wall of a building, and computing again returns 0);
// (2) it does not count other units as obstacles, yet units do block when "run to point" is executed (starting right next to Warriv: the first segment passes under his feet, the player does not move a step, and recomputing gives the same solution); (3) the game does not walk a single segment that is too long (a 56-cell segment: packet sent, player does not move).
// Why not just send "interact with unit" and let the original game walk: the player's default path type 7 stops on hitting an obstacle, so tapping an NPC across a wall / chest stops in front of the obstacle.
// The collision grid is read only: room {+0x80 x, +0x84 y, +0x88 width, +0x8c height (cells), +0x38 -> {+8 width, +0x20 u16 array}, +0 adjacent room array, +0x40 count} (the same data read by the game's line check main+0x4d1fc0).
// Runs on the game main thread at the controller-poll point (dm_world_pump); the arrays are all static and take no guest thread stack. Cost (measured on the Mac host): commonly 0.02–0.6 ms in town, worst case with an unreachable target 0.55–1.13 ms (searching 7 thousand-19 thousand cells);
// the expansion is capped at WP_CAP cells. When unreachable it computes only this once and cancels, without computing repeatedly; it recomputes once only after standing still for 1 second, at most 3 times.
#define WP_W 192   // 规划窗口（格），以起点与目标的中点为心；一屏可点范围约 ±45 格，够绕路。目标超出窗口不规划（只有测试开关会点到那么远） | planning window (cells), centered on the midpoint between start and target; the tappable range of one screen is about +-45 cells, enough for detours. No planning when the target is outside the window (only test switches tap that far)
#define WP_CAP 24000   // 一次规划最多展开这么多格 | one planning expands at most this many cells
#define WP_SEG 30   // 单段上限（格）：太长的「跑到点」游戏不走（实测：营地出生点 → 56 格外一段，包发了人不动；47 格那段走了） | per-segment limit (cells): the game does not walk a "run to point" that is too long (measured: camp spawn point -> one segment 56 cells away, the packet was sent and the player did not move; the 47-cell segment was walked)
static uint64_t g_wp_room[96]; static int g_wp_nroom;
static int wp_room_has(uint64_t r, int x, int y) { int rx = *(int *)(r + 0x80), ry = *(int *)(r + 0x84); return x >= rx && y >= ry && x < rx + *(int *)(r + 0x88) && y < ry + *(int *)(r + 0x8c); }
static void wp_rooms(uint64_t r0) {   // 从玩家所在房间按相邻关系铺开（已载入的房间） | spread out from the player's room by adjacency (loaded rooms)
    g_wp_nroom = 0; if (!r0) return; g_wp_room[g_wp_nroom++] = r0;
    for (int i = 0; i < g_wp_nroom; i++) { uint64_t r = g_wp_room[i], *adj = *(uint64_t **)r; uint32_t n = *(uint32_t *)(r + 0x40); for (uint32_t k = 0; adj && k < n && k < 32; k++) { uint64_t a = adj[k]; int seen = 0; if (!a) continue; for (int q = 0; q < g_wp_nroom; q++) if (g_wp_room[q] == a) { seen = 1; break; } if (!seen && g_wp_nroom < 96) g_wp_room[g_wp_nroom++] = a; } }
}
static int wp_cell(int x, int y) {   // 这一格的碰撞位；不在任何已载入房间里 = 全挡 | collision bits of this cell; not in any loaded room = fully blocked
    static int last; for (int t = 0; t < g_wp_nroom; t++) { int i = (last + t) % g_wp_nroom; uint64_t r = g_wp_room[i]; if (!wp_room_has(r, x, y)) continue; last = i;
        uint64_t g = *(uint64_t *)(r + 0x38); if (!g) return 0xffff; uint16_t *a = *(uint16_t **)(g + 0x20); int gw = *(int *)(g + 8); return a ? a[(y - *(int *)(r + 0x84)) * gw + (x - *(int *)(r + 0x80))] : 0xffff; }
    return 0xffff; }
// 哪些碰撞位挡玩家：照原版自己的口径——玩家路径结构 +0x64 的碰撞掩码（宿主实读 0x1c09 = 墙 0x1 + 0x8 + 物件 0x400 + 门 0x800 + 0x1000），规划时现读，读不到用这个值。
// 各类单位往格子上写的位（宿主实测，营地）：玩家 = 十字 0x80、中心格另加 0x1000；NPC = 十字 0x100、中心格另加 0x1000；佣兵 / 召唤物 = 3 × 3 的 0x100、十字另加 0x2000，没有 0x1000。
// 所以原版里挡玩家的是 NPC / 怪 / 别的玩家的「中心格」（0x1000），佣兵和召唤物不挡（0x100、0x2000 都不在掩码里）。原先这里按「0x100 = 别的单位」挡，
// 佣兵 / 召唤物叠在人身上时（进局落小站、传送、过门之后都这样）起点的八个邻格全被否，一步展不开 =「搜过 1 格，找不到路」（带佣兵的存档进局就中：点储藏箱取消）。
// 玩家自己的中心格也带 0x1000：原版走之前先把自己的占位从格子上拿掉再判，这里对起点格同样不算这一位
// Which collision bits block the player: follow the original game's own rule -- the collision mask at +0x64 of the player's path structure (read on the host as 0x1c09 = wall 0x1 + 0x8 + object 0x400 + door 0x800 + 0x1000), read at planning time, with this value used when it cannot be read.
// Bits that each kind of unit writes onto the cells (measured on the host, in the camp): player = cross 0x80, center cell additionally 0x1000; NPC = cross 0x100, center cell additionally 0x1000; mercenary / summons = 3 x 3 of 0x100, cross additionally 0x2000, no 0x1000.
// So in the original game what blocks the player is the "center cell" (0x1000) of NPCs / monsters / other players, while mercenaries and summons do not block (neither 0x100 nor 0x2000 is in the mask). This code used to block on "0x100 = another unit",
// and when a mercenary / summon overlapped the player (the case after landing on the waypoint on joining a run, after teleporting, after going through a door) all eight neighbors of the start cell were rejected and not a single step could expand = 「搜过 1 格，找不到路」(searched 1 cell, no route found) (a save with a mercenary hit it right on joining a run: tapping the stash cancelled).
// The player's own center cell also carries 0x1000: the original game removes its own occupancy from the cells before checking when it walks, and here this bit is likewise not counted for the start cell
#define WP_MASK_DEF 0x1c09
static uint32_t g_wp_mask = WP_MASK_DEF; static int g_wp_sx, g_wp_sy;   // 这一次规划用的掩码与起点格 | mask and start cell used for this planning
static uint8_t g_wp_blk[WP_W * WP_W]; static int g_wp_ox, g_wp_oy;   // 窗口内每格只查一次：0 没查过、1 不挡、2 挡 | each cell in the window is checked only once: 0 not checked, 1 not blocking, 2 blocking
static int wp_blk(int x, int y) { int ix = x - g_wp_ox, iy = y - g_wp_oy; if (ix < 0 || iy < 0 || ix >= WP_W || iy >= WP_W) return 1; uint8_t *m = &g_wp_blk[iy * WP_W + ix];
    if (!*m) { uint32_t v = (uint32_t)wp_cell(x, y) & g_wp_mask; if (x == g_wp_sx && y == g_wp_sy) v &= ~0x1000u; *m = v ? 2 : 1; } return *m == 2; }
static int wp_free(int x, int y) { return !(wp_blk(x, y) || wp_blk(x + 1, y) || wp_blk(x - 1, y) || wp_blk(x, y + 1) || wp_blk(x, y - 1)); }   // 玩家占一个十字（中心 + 四邻） | the player occupies a cross (center + four neighbors)
static int wp_line(int x0, int y0, int x1, int y1) {   // 两点连线上每一格都能站（含对角步的两个侧格） | every cell on the line between the two points can be stood on (including the two side cells of a diagonal step)
    int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx - dy;
    for (;;) { if (!wp_free(x0, y0)) return 0; if (x0 == x1 && y0 == y1) return 1; int e2 = 2 * e, mx = 0, my = 0; if (e2 > -dy) { e -= dy; mx = 1; } if (e2 < dx) { e += dx; my = 1; }
        if (mx && my && (!wp_free(x0 + sx, y0) || !wp_free(x0, y0 + sy))) return 0; if (mx) x0 += sx; if (my) y0 += sy; } }
// 越肩镜头避墙（dm_cam.c、画面线程 xbg_game.cpp）：每帧从人物脚下（路径结构 +0 / +4 的 16.16 定点子格坐标）在碰撞格上往 32 个方向走，
// 量出离第一面墙多远，镜头就放在墙前面。墙 = 碰撞位 0x1（挡走：墙、栅栏、树）或 0x800（门），单位与物件不算；不在已载入房间里也算墙。
// 3D 世界 = 子格 × 2，同轴（原版镜头看向世界 -x -z，正是子格的 -x -y），DM_CAM_SUB / DM_CAM_WALL 可改；量到 12 子格（24 世界单位，镜头缺省离人 20）为止，
// 停在墙前 0.75 子格。只读碰撞格，和路线规划一样在游戏主线程
// Over-the-shoulder camera wall avoidance (dm_cam.c, render thread xbg_game.cpp): every frame, walk the collision grid from the character's feet (16.16
// fixed-point sub-cell coordinates at +0 / +4 of the path structure) in 32 directions and measure how far the first wall is, so the camera can sit in
// front of it. A wall = collision bit 0x1 (blocks walking: walls, fences, trees) or 0x800 (doors); units and objects do not count, and outside the loaded
// rooms counts as wall. 3D world = sub-cell x 2 on the same axes (the original camera looks towards world -x -z, which is the sub-cells' -x -y);
// DM_CAM_SUB / DM_CAM_WALL change them. Measured out to 12 sub-cells (24 world units; the camera sits 20 from the character by default), stopping 0.75
// sub-cell short of the wall. Reads the collision grid only, on the game main thread like route planning
static void w_cam_clear(void) {
    int dm_cam_mode(void); void dm_cam_set_clear(const float d[32], float px, float pz);
    if (dm_cam_mode() != 1) return;
    uint64_t pl = w_player(), pp = pl ? *(uint64_t *)(pl + 0x38) : 0; if (!pp || !*(uint64_t *)(pp + 0x20)) return;
    static float sub = -1; static uint32_t mask;
    if (sub < 0) { const char *e = getenv("DM_CAM_SUB"); sub = e ? (float)atof(e) : 2.0f; e = getenv("DM_CAM_WALL"); mask = e ? (uint32_t)strtoul(e, NULL, 0) : 0x801; }
    float x0 = *(uint32_t *)pp / 65536.0f, y0 = *(uint32_t *)(pp + 4) / 65536.0f, d[32];
    wp_rooms(*(uint64_t *)(pp + 0x20));
    for (int k = 0; k < 32; k++) {
        float ux = cosf(k * 6.2831853f / 32), uy = sinf(k * 6.2831853f / 32), t = 0.5f;
        for (; t <= 12; t += 0.25f) if ((uint32_t)wp_cell((int)floorf(x0 + ux * t), (int)floorf(y0 + uy * t)) & mask) break;
        t -= 0.75f; d[k] = (t < 1 ? 1 : t) * sub; }
    dm_cam_set_clear(d, x0 * sub, y0 * sub);
}
static int w_plan(uint8_t *b, uint64_t pl, int x, int y) {
    (void)b; uint64_t t0 = w_now_ns(); uint64_t pp = *(uint64_t *)(pl + 0x38); g_go.n = g_go.i = 0; int r = 0; uint32_t nn = 0; if (!pp) return 0;
    int px = *(uint16_t *)(pp + 2), py = *(uint16_t *)(pp + 6); wp_rooms(*(uint64_t *)(pp + 0x20));
    { uint32_t mk = *(uint32_t *)(pp + 0x64); g_wp_mask = (mk & 1) && !(mk & ~0xffffu) ? mk : WP_MASK_DEF; g_wp_sx = px; g_wp_sy = py; static uint32_t said; if (g_wp_mask != said) { said = g_wp_mask; LOG("世界目标：规划用的碰撞掩码 %#x（玩家路径结构 +0x64 读到 %#x）", g_wp_mask, mk); } }
    static uint8_t from[WP_W * WP_W]; static uint32_t q[WP_W * WP_W]; static const int8_t D[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    int ox = (px + x) / 2 - WP_W / 2, oy = (py + y) / 2 - WP_W / 2, sxi = px - ox, syi = py - oy, goal = -1;
    if (abs(x - px) >= WP_W - 8 || abs(y - py) >= WP_W - 8) LOG("世界目标：目标离得太远（%d,%d 格），超出规划窗口，不规划", abs(x - px), abs(y - py));
    if (sxi >= 0 && syi >= 0 && sxi < WP_W && syi < WP_W && abs(x - px) < WP_W - 8 && abs(y - py) < WP_W - 8) {
        memset(from, 0, sizeof from); memset(g_wp_blk, 0, sizeof g_wp_blk); g_wp_ox = ox; g_wp_oy = oy; int h = 0, t = 0; q[t++] = (uint32_t)(syi * WP_W + sxi); from[syi * WP_W + sxi] = 9;
        int best = -1, bestd = 1 << 30;
        while (h < t && h < WP_CAP) { int c = (int)q[h++], cx = c % WP_W + ox, cy = c / WP_W + oy; if (abs(cx - x) <= W_NEAR && abs(cy - y) <= W_NEAR) { goal = c; break; }
            { int ddx = abs(cx - x), ddy = abs(cy - y), dd = (ddx > ddy ? ddx : ddy) * 64 + ddx + ddy; if (dd < bestd) { bestd = dd; best = c; } }
            for (int d = 0; d < 8; d++) { int nx = cx + D[d][0], ny = cy + D[d][1], ix = nx - ox, iy = ny - oy; if (ix < 0 || iy < 0 || ix >= WP_W || iy >= WP_W || from[iy * WP_W + ix]) continue;
                if (!wp_free(nx, ny)) continue; if (d >= 4 && (!wp_free(cx + D[d][0], cy) || !wp_free(cx, cy + D[d][1]))) continue; from[iy * WP_W + ix] = (uint8_t)(d + 1); q[t++] = (uint32_t)(iy * WP_W + ix); } }
        r = goal >= 0; nn = (uint32_t)t;
        // 目标格走不到（物件自己和它周围被挡着：关着的大门，如亚瑞特山顶通往世界之石要塞那扇，能站的最近处离它 6 格）而能到的格子已搜完：改走到「能到的格子里离它最近的那格」，
        // 到了由 w_go_step 就地交互（只对物件）。原版手柄主钮在那个位置就能开这扇门（anc1–anc4：手指 8 下全是「找不到路，取消」，主钮 1 下开）
        // The target cell cannot be reached (the object itself and its surroundings are blocked: a closed gate, such as the one from Arreat Summit to the Worldstone Keep, where the nearest standable spot is 6 cells from it) and the reachable cells have all been searched: walk instead to "the reachable cell closest to it",
        // and on arrival w_go_step interacts in place (objects only). The original game's controller primary button opens this gate from that position (four regression cases: 8 finger taps were all 「找不到路，取消」(no route found, cancel), 1 press of the primary button opened it)
        g_go.nearest = 0; if (goal < 0 && h >= t && best >= 0 && W_NEAREST_OK(g_go.type)) { g_go.nearest = 1; if (best != syi * WP_W + sxi) goal = best; }
        if (goal >= 0) {   // 回溯成格子序列（复用 q 的尾部），再拉直成几段直线 | backtrack into a cell sequence (reusing the tail of q), then straighten it into a few straight segments
            int n = 0, c = goal; while (from[c] != 9 && n < WP_W * WP_W / 2) { q[WP_W * WP_W - 1 - n++] = (uint32_t)c; int d = from[c] - 1; c -= D[d][1] * WP_W + D[d][0]; }
            uint32_t *path = q + WP_W * WP_W - n; int ax = px, ay = py, k = 0;
            while (k < n && g_go.n < 24) { int best = k; for (int m = (n - 1 < k + WP_SEG ? n - 1 : k + WP_SEG); m > k; m--) if (wp_line(ax, ay, (int)(path[m] % WP_W) + ox, (int)(path[m] / WP_W) + oy)) { best = m; break; }   // 一段最多 WP_SEG 步 | at most WP_SEG steps per segment
                ax = (int)(path[best] % WP_W) + ox; ay = (int)(path[best] / WP_W) + oy; g_go.node[g_go.n][0] = (uint16_t)ax; g_go.node[g_go.n][1] = (uint16_t)ay; g_go.n++; k = best + 1; }
            if (k < n) g_go.n = 0; } }
    // 一步都没展开出去（起点的八个邻格全被否）：记起点周围 5 × 5 格的碰撞位（十六进制，一行一个 y），看是什么占着。测试开关 DM_TEST_WP_DUMP 每次规划都记（11 × 11），并记路线各段终点
    // Not a single step expanded (all eight neighbors of the start cell were rejected): log the collision bits of the 5 x 5 cells around the start (hexadecimal, one y per line) to see what is occupying them. The test switch DM_TEST_WP_DUMP logs on every planning (11 x 11) and also logs the end point of each route segment
    if ((nn <= 1 && !r) || getenv("DM_TEST_WP_DUMP")) { static int dl; int R = getenv("DM_TEST_WP_DUMP") ? 5 : 2; if (dl++ < 12) { char o[800]; size_t L = 0; o[0] = 0;
        for (int yy = py - R; yy <= py + R; yy++) { for (int xx = px - R; xx <= px + R; xx++) L += (size_t)snprintf(o + L, L < sizeof o ? sizeof o - L : 0, "%4x ", wp_cell(xx, yy)); L += (size_t)snprintf(o + L, L < sizeof o ? sizeof o - L : 0, "| "); }
        LOG("世界目标：起点 %d,%d 周围碰撞位 %s", px, py, o); } }
    if (getenv("DM_TEST_WP_DUMP") && g_go.n) { char o[300]; size_t L = 0; o[0] = 0; for (int i = 0; i < g_go.n; i++) L += (size_t)snprintf(o + L, L < sizeof o ? sizeof o - L : 0, " %d,%d", g_go.node[i][0], g_go.node[i][1]); LOG("世界目标：路线各段终点%s", o); }
    g_go.tx = x; g_go.ty = y;
    static int lg; if (lg++ < 60) LOG("世界目标：路线 %d 段（找到 %d、搜过 %u 格、%.2f ms；末点 %d,%d；目标 %d,%d）", g_go.n, r, nn, (double)(w_now_ns() - t0) / 1e6, g_go.n ? g_go.node[g_go.n - 1][0] : 0, g_go.n ? g_go.node[g_go.n - 1][1] : 0, x, y);
    return g_go.n;
}
// 物件要求玩家和它在同一个房间里才生效的情况（鲁高因下水道活板门：操作命令在隔壁房间也被受理、门会开，但传送那一步不发生——真机与宿主实测：
// 站它南侧 3 格（同房间）能进，站东侧 3 格（x 跨过房间边界）连点四次都不进）。房间指针：物件在路径结构 [单位+0x38] 的 +0，玩家（会走动的单位）在 +0x20（运行时读出：人站到活板门同侧时两者相等，跨过 x = 5040 就不等）
// The case where an object requires the player to be in the same room for it to take effect (Lut Gholein sewer trapdoor: the operate command is accepted from the neighboring room too and the door opens, but the teleport step does not happen -- measured on real device and host:
// standing 3 cells south of it (same room) gets in, standing 3 cells east (x crosses the room border) does not get in even after four taps). Room pointer: for an object at +0 of the path structure [unit+0x38], for the player (a walking unit) at +0x20 (read at run time: the two are equal when the player stands on the trapdoor's side and differ once past x = 5040)
static int w_same_room(uint64_t pl, uint64_t u) { uint64_t a = *(uint64_t *)(pl + 0x38), c = *(uint64_t *)(u + 0x38); return a && c && *(uint64_t *)c && *(uint64_t *)(a + 0x20) == *(uint64_t *)c; }
// 只有「操作时在物件自己的房间里找传送砖」的那种物件才有同房间的要求：物件表记录（[版本表 + 0x1548] + 类号 × 0x168）+0x15e = 操作函数号，16 号（活板门，main+0x6aeaa0）是这么做的。
// 别的物件（小站、箱子…）跨房间照样能操作，不为它们绕路
// Only objects of the kind that "look for the teleport tile in the object's own room when operated" have the same-room requirement: objects table record ([version table + 0x1548] + class * 0x168) +0x15e = operate function number, and number 16 (trapdoor, main+0x6aeaa0) does it this way.
// Other objects (waypoints, chests...) can be operated across rooms all the same, so no detour is made for them
static int w_needs_room(uint64_t u) { uint8_t *b = wb(); if (*(uint32_t *)u != 2) return 0; uint8_t ver = *(uint8_t *)(u + 0x1bd); if (ver < 1 || ver > 3) return 0;
    uint64_t t = *(uint64_t *)(b + 0x2d64a18 + (uint64_t)ver * 16); uint32_t cls = *(uint32_t *)(u + 4); if (!t || cls >= *(uint64_t *)(t + 0x1550)) return 0;
    return *(uint8_t *)(*(uint64_t *)(t + 0x1548) + (uint64_t)cls * 0x168 + 0x15e) == 16; }
static struct { uint32_t id; uint64_t lv, at; int x, y, tries; } g_tile_retry;   // 对出口砖交互过、等着看换没换关卡（见 w_arrive） | interacted with the exit tile and waiting to see whether the level changed (see w_arrive)
static uint64_t w_level(uint64_t pl) { uint64_t pp = pl ? *(uint64_t *)(pl + 0x38) : 0, r1 = pp ? *(uint64_t *)(pp + 0x20) : 0, r2 = r1 ? *(uint64_t *)(r1 + 0x18) : 0; return r2 ? *(uint64_t *)(r2 + 0x90) : 0; }
static void w_tile_retry_step(void) {
    if (!g_tile_retry.at || dm_guest_frame() < g_tile_retry.at) return; g_tile_retry.at = 0; uint8_t *b = wb(); uint64_t pl = w_player(); int px = 0, py = 0;
    if (!b || !pl || g_go.on || w_level(pl) != g_tile_retry.lv || !w_unit_xy(pl, &px, &py) || abs(px - g_tile_retry.x) > W_FAR || abs(py - g_tile_retry.y) > W_FAR) return;   // 换了关卡 / 人走开了 / 又点了别的：不管 | level changed / player walked away / something else was tapped: ignore
    uint64_t act = 0; long bd = -1; int ax = 0, ay = 0, ox = g_tile_retry.x, oy = g_tile_retry.y;
    for (int k = 0; k < 128; k++) for (uint64_t c = *(uint64_t *)(b + 0x2d603a0 + 2 * 0x400 + k * 8); c; c = *(uint64_t *)(c + 0x158)) { int cx = 0, cy = 0;
        if (!w_unit_xy(c, &cx, &cy) || abs(cx - ox) > 2 || abs(cy - oy) > 2) continue; if (!(((int (*)(uint64_t, int, int, int, int))(b + 0x464a70))(c, 0, 0, -1, -1) & 1) || ((int (*)(int, uint64_t, uint64_t))(b + 0x2f7b7c))(0x165, pl, c) != 2) continue;
        long d = (long)(cx - ox) * (cx - ox) + (long)(cy - oy) * (cy - oy); if (bd < 0 || d < bd) { bd = d; act = c; ax = cx; ay = cy; } }
    // 旁边没有把守它的物件：再对出口砖交互，最多补两次。人刚走到 / 刚被挪到就发的那一下服务端有时不认（水晶通道 → 冰冻之河同一站位 2 局 1 进 1 不进；探路「传到旁边立刻交互」也撞过），站定之后再发就认
    // No object guarding it nearby: interact with the exit tile again, at most two more times. The one sent right after the player walked up / was just moved there is sometimes not accepted by the server (Crystalline Passage -> Frozen River, same standing spot, 2 runs: 1 entered, 1 did not; the path probe's "teleport next to it and interact immediately" hit it too), and sending again after standing still is accepted
    if (!act) { uint64_t tu = w_find(5, g_tile_retry.id); if (!tu || g_tile_retry.tries >= 2) { LOG("世界目标：出口砖 id %u 交互 %d 次后仍在原关卡，2 格内没有可交互的物件，不再试", g_tile_retry.id, g_tile_retry.tries + 1); return; }
        g_tile_retry.tries++; g_tile_retry.at = dm_guest_frame() + 45; int r = w_interact(tu); LOG("世界目标：出口砖 id %u 交互后 45 帧仍在原关卡，旁边没有把守物件 → 再交互一次（第 %d 次补发）→ 交互 %d", g_tile_retry.id, g_tile_retry.tries, r); return; }
    int r = w_interact(act); LOG("世界目标：出口砖 id %u 交互后 45 帧仍在原关卡 → 改交互把守它的物件 cls %u id %u（%d,%d）→ 交互 %d", g_tile_retry.id, *(uint32_t *)(act + 4), *(uint32_t *)(act + 8), ax, ay, r);
}
// 到了能交互的地方：有同房间要求的物件而人不在它的房间里时，换到物件的南 / 北 / 西 / 东各 3 格处再试（走到了且同房间才交互），四侧都不行才照旧就地交互。返回 1 = 这次走近结束
// At a place where interaction is possible: for an object with a same-room requirement when the player is not in its room, move to 3 cells south / north / west / east of the object and try again (interact only once arrived and in the same room), and only if all four sides fail interact in place as before. Returns 1 = this approach is over
static int w_arrive(uint64_t pl, uint64_t u, int px, int py, int ox, int oy, const char *how) {
    static const int off[4][2] = {{0, 3}, {0, -3}, {-3, 0}, {3, 0}}; int ok = g_go.stop_at || !w_needs_room(u) || w_same_room(pl, u);
    if (!ok && g_go.alt < 4) { g_go.ax = ox + off[g_go.alt][0]; g_go.ay = oy + off[g_go.alt][1]; g_go.alt++; g_go.alt_on = 1; g_go.n = g_go.i = g_go.replans = g_go.empty = g_go.direct = 0; g_go.next_fr = 0; g_go.still_fr = 0; g_go.until_ns = w_now_ns() + 8000000000ull;
        LOG("世界目标：类型 %u id %u 在（玩家 %d,%d 目标 %d,%d）和物件不在同一房间，换到 (%d,%d) 再试（第 %d 侧）", g_go.type, g_go.id, px, py, ox, oy, g_go.ax, g_go.ay, g_go.alt); return 0; }
    // 出口由门 / 楼梯物件把守时，要交互的是那个物件，不是出口砖：崔凡克 → 憎恨囚牢，对出口砖发交互（0x40）服务端不换关卡，原生主钮在同一位置选的是旁边的楼梯物件 386（0x41），能进。
    // 所以点的是出口砖时，到了跟前问一次游戏自己的「当前交互目标」（main+0x2f7a0c，主钮用的就是它）：它选了出口砖 3 格内的一个物件，就和主钮一样交互那个物件；否则照旧交互出口砖（法师峡谷的古墓入口没有这种物件）
    // When an exit is guarded by a door / stairs object, the thing to interact with is that object, not the exit tile: Travincal -> Durance of Hate, sending the interaction (0x40) to the exit tile the server does not change level, while the native primary button at the same position selects the stairs object 386 next to it (0x41) and gets in.
    // So when the exit tile is what was tapped, on arriving ask the game once for its own "current interaction target" (main+0x2f7a0c, exactly what the primary button uses): if it selected an object within 3 cells of the exit tile, interact with that object just like the primary button; otherwise interact with the exit tile as before (the tomb entrances in the Canyon of the Magi have no such object)
    uint64_t act = u; uint8_t *b = wb();
    if (!g_go.stop_at && b && *(uint32_t *)u == 5) { uint64_t nt = ((uint64_t (*)(uint64_t, uint32_t))(b + 0x2f7a0c))(((uint64_t (*)(void))(b + 0x2acecc))(), *(uint32_t *)(b + 0x2d20e4c)); int nx = 0, ny = 0;
        if (nt && nt != u && *(uint32_t *)nt == 2 && w_unit_xy(nt, &nx, &ny) && abs(nx - ox) <= 3 && abs(ny - oy) <= 3) act = nt;
        // 游戏的这个目标逐帧刷新、人刚停下时常是空的（同一现场 5 局里 2 局空，等 20 帧仍空）：空的时候先照旧对出口砖交互，45 帧后人还在原关卡再去找把守它的物件（w_tile_retry_step）——
        // 能进的出口当场就进了，旁边碰巧有箱子也不相干（按结果收窄，不按数据猜）
        // This target of the game is refreshed every frame and is often empty right after the player stops (empty in 2 of 5 runs of the same setup, still empty after waiting 20 frames): when it is empty, first interact with the exit tile as before, and if the player is still in the original level after 45 frames go look for the object guarding it (w_tile_retry_step) --
        // an exit that can be entered is entered on the spot, and a chest that happens to be nearby does not matter (narrow down by result, do not guess from data)
        if (act == u) { g_tile_retry.id = g_go.id; g_tile_retry.lv = w_level(pl); g_tile_retry.at = dm_guest_frame() + 45; g_tile_retry.x = ox; g_tile_retry.y = oy; g_tile_retry.tries = 0; }
        if (act != u) LOG("世界目标：出口砖 id %u 旁有把守它的物件 cls %u id %u（%d,%d，游戏自己选中的）→ 交互它", g_go.id, *(uint32_t *)(act + 4), *(uint32_t *)(act + 8), nx, ny); }
    g_go.on = 0; int r = w_interact(act); LOG("世界目标：类型 %u id %u %s（玩家 %d,%d 目标 %d,%d%s）→ 交互 %d", g_go.type, g_go.id, how, px, py, ox, oy, ok ? "" : "，四侧都进不了同一房间", r); return 1;
}
#if TARGET_OS_OSX
// ---- Mac 鼠标：点空地走过去 ----
// 触屏上「除摇杆外触点不触发点地移动」（见文件头）不变；这一段只给 Mac App 的鼠标用：键鼠玩家没有摇杆，点空地 = 走到那一点，按住 = 一直朝光标走（touch_mac.c 每隔几拍重发一次）。
// 屏幕点 → 地面格：以玩家脚底的屏幕位置（游戏自己的投影）为原点，按等距投影反算——每「经典像素」合屏幕 k = 画面高 × 0.00206 像素（同上面物件占地菱形用的那个数），
// 一格沿 x 方向在屏幕上是右 16、下 8 经典像素，沿 y 方向是左 16、下 8：格差 dx = (横/16 + 纵/8) / 2、dy = (纵/8 − 横/16) / 2。画面有透视，离人越远偏得越多（验收时量）。
// 走法复用上面的路线规划与逐段「跑到点」；到点 = 离目标格 1 格内；目标格站不了人（墙里、水里）= 走到能到的最近一格就停；原地 1 秒不动重算一次，再不动就停；15 秒没到停。
// 摇杆（含键盘 W A S D）一动就让给摇杆：进行中的取消，0.3 秒内的新请求不受理。
// ---- Mac mouse: click empty ground to walk there ----
// On touch, "touches other than the stick do not trigger click-to-move" (see the file header) is unchanged; this section is only for the Mac App's mouse: keyboard-and-mouse players have no stick, so a click on empty ground = walk to that point, holding = keep walking towards the pointer (touch_mac.c re-sends every few ticks).
// Screen point -> ground tile: with the screen position of the player's feet (the game's own projection) as origin, back-compute through the isometric projection - each "classic pixel" is k = picture height x 0.00206 screen pixels (the same number used above for the footprint diamond of objects),
// one tile along x is 16 right and 8 down in classic pixels on screen, along y 16 left and 8 down: tile difference dx = (horizontal/16 + vertical/8) / 2, dy = (vertical/8 - horizontal/16) / 2. The picture has perspective, so the further from the character the larger the error (measured during acceptance).
// Walking reuses the route planning above and the segment-by-segment "run to point"; arrived = within 1 tile of the target tile; a target tile that cannot be stood on (inside a wall, in water) = walk to the nearest reachable tile and stop; no movement for 1 second: re-plan once, then stop if still not moving; stop after 15 seconds without arriving.
// As soon as the stick (including keyboard W A S D) moves, it yields to the stick: a walk in progress is cancelled and new requests within 0.3 s are not accepted.
static _Atomic uint64_t g_walk;
void dm_world_walk(float nx, float ny) { if (nx < 0) nx = 0; if (nx > 1) nx = 1; if (ny < 0) ny = 0; if (ny > 1) ny = 1; atomic_store(&g_walk, 1ull << 32 | (uint64_t)(uint32_t)(nx * 65535) << 16 | (uint32_t)(ny * 65535)); }   // 任意线程：排一笔，主线程取手柄处执行；后一笔覆盖前一笔 | any thread: queue one request, executed where the main thread reads the gamepad; a later request overwrites an earlier one
int dm_world_walking(void) { return g_go.on && g_go.type == W_GROUND; }
static void w_ground_end(const char *how, int px, int py) { g_go.on = 0; static int lg; if (lg++ < 400) LOG("点地面走：%s（玩家 %d,%d 目标 %d,%d）", how, px, py, g_gnd_x, g_gnd_y); }
static void w_ground_step(uint8_t *b, uint64_t pl) {
    uint64_t dm_stick_last_ns(void); uint32_t dm_cur_tick(void); int px = 0, py = 0;
    if (!pl || !w_unit_xy(pl, &px, &py)) { g_go.on = 0; return; }
    if (dm_stick_last_ns() != g_go.stick) { w_ground_end("取消（摇杆 / W A S D 动了）", px, py); return; }
    if (w_now_ns() > g_go.until_ns) { w_ground_end("停下（15 秒没到）", px, py); return; }
    int x = g_gnd_x, y = g_gnd_y, dx = abs(x - px), dy = abs(y - py); uint64_t f = dm_cur_tick(), fr = dm_guest_frame();
    if (dx <= 1 && dy <= 1) { w_ground_end("到点", px, py); return; }
    if (px != g_go.lx || py != g_go.ly || !g_go.still_fr) { g_go.lx = px; g_go.ly = py; g_go.still_fr = f; }
    else if (g_go.next_fr && f - g_go.still_fr > 25) {   // 发过令而原地 1 秒没动 | an order was issued and it has not moved for 1 second
        if (g_go.direct || g_go.replans++ >= 1) { w_ground_end(g_go.nearest ? "停在能到的最近处" : "停下（走不动了）", px, py); return; }
        g_go.n = 0; g_go.next_fr = 0; g_go.still_fr = f; }
    if (!g_go.direct && !g_go.n) { if (!w_plan(b, pl, x, y)) { w_ground_end("不走（找不到路）", px, py); return; } g_go.next_fr = 0; }
    while (g_go.i < g_go.n && abs((int)g_go.node[g_go.i][0] - px) <= 1 && abs((int)g_go.node[g_go.i][1] - py) <= 1) { g_go.i++; g_go.next_fr = 0; }
    int gx, gy;
    if (g_go.i >= g_go.n) {   // 路线走完：规划的终点在目标 2 格内，最后朝目标格直走一次；目标格本来就站不了人的到此为止 | route finished: the planned end point is within 2 tiles of the target, so walk straight to the target tile once at the end; if the target tile cannot be stood on, stop here
        if (g_go.nearest || !wp_line(px, py, x, y)) { w_ground_end(g_go.nearest ? "停在能到的最近处" : "到点（目标格旁）", px, py); return; }
        if (g_go.direct && fr < g_go.next_fr) return; g_go.direct = 1; gx = x; gy = y; }
    else { gx = g_go.node[g_go.i][0]; gy = g_go.node[g_go.i][1]; }
    if (fr < g_go.next_fr) return; g_go.next_fr = fr + 20;
    atomic_store(&g_dm_move_allow_until_ns, w_now_ns() + 500000000ull);
    ((void (*)(int, uint64_t, int, int))(b + 0x4ad870))(3, pl, gx, gy);
}
#endif
static void w_go_step(void) {
    if (!g_go.on) return; uint64_t dm_stick_last_ns(void); uint8_t *b = wb(); uint64_t pl = w_player(), u = pl ? w_find(g_go.type, g_go.id) : 0; int px, py, x, y;
#if TARGET_OS_OSX
    if (g_go.type == W_GROUND) { w_ground_step(b, pl); return; }
#endif
    const char *why = !u ? "目标没了" : w_now_ns() > g_go.until_ns ? "15 秒没到" : dm_stick_last_ns() != g_go.stick ? "摇杆动了" : !w_unit_xy(pl, &px, &py) || !w_unit_xy(u, &x, &y) ? "读不到坐标" : (g_go.type == 4 && *(uint32_t *)(u + 0xc) != 3) ? "物品已不在地上" : NULL;
    if (why) { g_go.on = 0; LOG("世界目标：类型 %u id %u 取消（%s）", g_go.type, g_go.id, why); return; }
    int ox = x, oy = y; if (g_go.alt_on) { x = g_go.ax; y = g_go.ay; }   // 换侧时走向的是物件旁的那一点；ox / oy 仍是物件自己 | when switching sides the walk goes to the point beside the object; ox / oy are still the object itself
    int dx = x - px, dy = y - py; if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
    // 走着走着被挡住（规划之后走过来的 NPC、规划没算到的东西）：原地 1 秒没动就重算路线（单位位置在障碍里，重算会换解），重算 3 次还不动才放弃
    // Blocked while walking (an NPC that walked over after planning, something the plan did not account for): after 1 second without moving, recompute the route (unit positions are in the obstacles, so recomputing gives a different solution), and give up only if still not moving after 3 recomputations
    { uint32_t dm_cur_tick(void); uint64_t f = dm_cur_tick(); if (px != g_go.lx || py != g_go.ly || !g_go.still_fr) { g_go.lx = px; g_go.ly = py; g_go.still_fr = f; } else if (g_go.next_fr && f - g_go.still_fr > 25) { if (!g_go.stop_at && dx <= W_FAR && dy <= W_FAR) { w_arrive(pl, u, px, py, ox, oy, "走不进 2 格，就地"); return; } if (g_go.replans++ < 3) { g_go.n = 0; g_go.next_fr = 0; g_go.still_fr = f; } else if (g_go.alt_on) { w_arrive(pl, u, px, py, ox, oy, "换侧走不到，就地"); return; } else { g_go.on = 0; LOG("世界目标：类型 %u id %u 取消（走不过去，停在 %d,%d）", g_go.type, g_go.id, px, py); return; } } }   // 按服务端拍计（50 拍 = 2 秒），不按出图帧：快进测试里帧比拍多得多 | counted in server ticks (50 ticks = 2 seconds), not presented frames: in fast-forward tests there are far more frames than ticks
    if (g_go.stop_at > 0 && dx <= g_go.stop_at && dy <= g_go.stop_at) { g_go.on = 0; ((void (*)(int, uint64_t, int, int))(b + 0x4ad870))(3, pl, px, py); LOG("世界目标：类型 %u id %u 走到旁边（玩家 %d,%d 目标 %d,%d），不交互", g_go.type, g_go.id, px, py, x, y); return; }
    // 出口砖要走到 1 格内才交互：斜对角 2 格处发交互（0x40）服务端不换关卡（水晶通道 → 冰冻之河 5/5 局：玩家 10373,5200、出口砖 10371,5198；差 (1,2)、(1,0) 的两处实测能进）。
    // 2 格内而还没到 1 格时不在这里到达，落到下面「已在 4 格内 → 朝它直走一次」那条；真走不进 1 格的（原地停 1 秒）仍由上面的「就地」兜底
    // An exit tile is interacted with only from within 1 cell: sending the interaction (0x40) from 2 cells away diagonally, the server does not change level (Crystalline Passage -> Frozen River 5/5 runs: player 10373,5200, exit tile 10371,5198; the two spots at offsets (1,2) and (1,0) were measured to get in).
    // Within 2 cells but not yet within 1, do not arrive here; fall through to the "already within 4 cells -> walk straight toward it once" rule below; one that really cannot get within 1 cell (standing still for 1 second) is still caught by the "in place" rule above
    int near = g_go.type == 5 ? 1 : W_NEAR;
    if (!g_go.stop_at && dx <= near && dy <= near) { w_arrive(pl, u, px, py, ox, oy, "到跟前"); return; }
    if (!g_go.stop_at && g_go.type == 5 && dx <= W_NEAR && dy <= W_NEAR && !g_go.direct) { g_go.n = g_go.i = 0; g_go.direct = 1; g_go.still_fr = 0; g_go.next_fr = dm_guest_frame() + 20; atomic_store(&g_dm_move_allow_until_ns, w_now_ns() + 500000000ull); ((void (*)(int, uint64_t, int, int))(b + 0x4ad870))(3, pl, x, y); return; }
    if (!g_go.stop_at && g_go.type == 5 && g_go.direct) return;   // 直走那一步进行中：等到 1 格内或原地停 1 秒 | the straight-walk step is in progress: wait until within 1 cell or standing still for 1 second
    // 没有路线、或目标（会走动的 NPC）离规划时的位置超过 6 格 → 重新规划
    // No route, or the target (a walking NPC) is more than 6 cells from its position at planning time -> replan
    if (!g_go.direct && (!g_go.n || abs(x - g_go.tx) > 6 || abs(y - g_go.ty) > 6)) { w_plan(b, pl, x, y); g_go.next_fr = 0; }
    while (g_go.i < g_go.n && abs((int)g_go.node[g_go.i][0] - px) <= 1 && abs((int)g_go.node[g_go.i][1] - py) <= 1) { g_go.i++; g_go.next_fr = 0; }   // 到了当前节点 → 下一段 | reached the current node -> next segment
    // 没有节点可走时不朝目标发直线移动（那会顶着墙跑）。路线走完了还没到跟前（目标走开了）→ 从当前位置重新规划；规划不出路 → 已在 4 格内就地交互，否则原地取消
    // When there is no node to walk to, do not send a straight-line move toward the target (that would run against a wall). Route finished but still not next to it (the target walked away) -> replan from the current position; no route can be planned -> interact in place if already within 4 cells, otherwise cancel on the spot
    if (g_go.i >= g_go.n) {
        if (g_go.n && g_go.empty++ < 8) { g_go.n = 0; g_go.next_fr = 0; return; }
        // 已在 4 格内而寻路给不出更近的点（目标自己占着格子，如第二幕下水道活板门：路线末点离它 3 格，在 3 格处交互服务端不认）：只这几格朝它直走一次，
        // 走到 2 格内或原地停 1 秒后由上面两条判定去交互。离得远时不直走（会顶墙）
        // Already within 4 cells and pathfinding yields no closer point (the target itself occupies the cell, e.g. the act 2 sewer trapdoor: the route's last point is 3 cells from it, and the server does not accept an interaction from 3 cells): walk straight toward it once for just these few cells,
        // and after getting within 2 cells or standing still for 1 second the two rules above decide on the interaction. Do not walk straight when far away (it would push against walls)
        if (!g_go.stop_at && dx <= W_FAR && dy <= W_FAR) { if (!g_go.direct) { g_go.direct = 1; g_go.still_fr = 0; g_go.next_fr = dm_guest_frame() + 20; atomic_store(&g_dm_move_allow_until_ns, w_now_ns() + 500000000ull); ((void (*)(int, uint64_t, int, int))(b + 0x4ad870))(3, pl, x, y); } return; }
        if (g_go.alt_on) { w_arrive(pl, u, px, py, ox, oy, "换侧走不到，就地"); return; }
        if (g_go.nearest && !g_go.stop_at && g_go.type == 2 && dx <= 8 && dy <= 8) { w_arrive(pl, u, px, py, ox, oy, "已在能到的最近处，就地"); return; }   // 见 w_plan：目标格被物件自己挡着 | see w_plan: the target cell is blocked by the object itself
        g_go.on = 0;
        LOG("世界目标：类型 %u id %u 取消（找不到路，停在 %d,%d，目标 %d,%d）", g_go.type, g_go.id, px, py, x, y);
        return; }
    g_go.empty = 0; int gx = g_go.node[g_go.i][0], gy = g_go.node[g_go.i][1];
    uint64_t fr = dm_guest_frame(); if (fr < g_go.next_fr) return; g_go.next_fr = fr + 20;
    atomic_store(&g_dm_move_allow_until_ns, w_now_ns() + 500000000ull);
    ((void (*)(int, uint64_t, int, int))(b + 0x4ad870))(3, pl, gx, gy);
}
// ---- 屏幕点 → 目标 ----
// 单位 → 屏幕：游戏自己的投影 main+0x90d110(场景 [main+0x2e2a698], 单位, float out[2], 0 脚底 / 1 头顶)（物品名标签定位用的就是它，0x43b62c）；
// 物品名标签：标签管理器 main+0x2f0fee0（{+0 记录数组, +8 条数}，每条 0x148：x y w h（屏幕像素）、+0x10 物品 id；绘制函数 0xc0189c 每帧重建）。
// 这版里键鼠的「鼠标悬停选单位 / 点标签」命中代码没有可用入口（0x464a70 的坐标模式无人调用），所以在这里做：
// 可交互的判定用游戏自己的：可选中（main+0x464a70(单位, 0, 0, -1, -1)）+ 交互类别（main+0x2f7b7c(0x165, 玩家, 单位)：5 = NPC、2 = 物件、3 = 物品、4 = 出口、9 = 尸体）。
// 敌对怪物（类型 1 且类别不是 5）不处理：攻击归技能键。
// 命中 = 触点落在目标自身的屏幕矩形内（改口径：吸附只补手指误差，不让空地变成 NPC——真实触点测到离 NPC 50–73 像素的空地被认成点他）。
// 矩形：单位取「脚底—头顶」为高、宽按高的比例（人形 0.44、物件 1.0）的身体框，物件再并上占地范围（见下）；地上物品取名字标签矩形，没有标签时取脚底点；
// 任一边不足 min（约 7 毫米）的补到 min 为止，不再整体外扩。多个矩形都含触点时取中心离触点最近的。
// ---- screen point -> target ----
// Unit -> screen: the game's own projection main+0x90d110(scene [main+0x2e2a698], unit, float out[2], 0 feet / 1 head) (it is what positions item name labels, 0x43b62c);
// Item name labels: label manager main+0x2f0fee0 ({+0 record array, +8 count}, each entry 0x148: x y w h (screen pixels), +0x10 item id; rebuilt every frame by the draw function 0xc0189c).
// In this version the keyboard-and-mouse "mouse hover selects unit / click label" hit code has no usable entry (nobody calls the coordinate mode of 0x464a70), so it is done here:
// Whether something is interactable uses the game's own checks: selectable (main+0x464a70(unit, 0, 0, -1, -1)) + interaction category (main+0x2f7b7c(0x165, player, unit): 5 = NPC, 2 = object, 3 = item, 4 = exit, 9 = corpse).
// Hostile monsters (type 1 and category not 5) are not handled: attacking belongs to the skill buttons.
// Hit = the touch point falls inside the target's own screen rectangle (criterion changed: snapping only makes up for finger error and does not turn empty ground into an NPC -- real touch points on empty ground 50–73 pixels from an NPC were measured as taps on him).
// Rectangle: a unit takes a body box with "feet-to-head" as height and width proportional to the height (humanoid 0.44, object 1.0), objects also union in the footprint (see below); an item on the ground takes its name label rectangle, or the feet point when there is no label;
// any side shorter than min (about 7 mm) is padded up to min, with no further overall expansion. When several rectangles contain the touch point, take the one whose center is closest to the touch point.
int dm_cam_on(void); int dm_cam_tap_for_unit(float tx, float ty, float fx, float fy, float *ox, float *oy); int dm_cam_unit_to_screen(float fx, float fy, float hx, float hy, float *ox, float *oy);   // dm_cam.c
// 镜头实验要不要自己换算触点：dm_uiscreen.c 的观测门在的话，w_project 走的游戏投影已经给透视画面上的坐标（dm_cam_label_matrix），
// 候选的框本来就在玩家看见的地方，触点直接比；只有老产物（没有这道门）才走 dm_cam_tap_for_unit / dm_cam_unit_to_screen 的换算
// Whether the camera experiment converts taps itself: when dm_uiscreen.c's observation gate exists, the game projection behind w_project already gives
// perspective picture coordinates (dm_cam_label_matrix), so the candidates' boxes are where the player sees them and the tap is compared directly; only
// older artifacts (without that gate) take the dm_cam_tap_for_unit / dm_cam_unit_to_screen conversion
int dm_ui_proj_gate(void);
static int w_cam_convert(void) { return dm_cam_on() && !dm_ui_proj_gate(); }
static int w_project(uint8_t *b, uint64_t scene, uint64_t u, float *out, int top) { int r = ((int (*)(uint64_t, uint64_t, float *, int))(b + 0x90d110))(scene, u, out, top); if (r) out[1] += dm_ui_proj_dy(); return r; }   // 这里要真实画面坐标：把界面矩形的 y 加回来（见 dm_uiscreen.c） | real picture coordinates are needed here: add the UI rectangle's y back (see dm_uiscreen.c)
static int w_in_rect(float px, float py, float cx, float cy, float w, float h, float min, float *d) {
    if (w < min) w = min; if (h < min) h = min; float dx = px - cx, dy = py - cy; if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
    if (dx > w / 2 || dy > h / 2) return 0; *d = sqrtf(dx * dx + dy * dy); return 1;
}
// 游戏自己的「点是否落在单位上」（main+0x2996b0，键鼠版鼠标选单位用；这版没人调它，照它读同一份数据）：单位 +0x78 → {+0x28 条目数组, +0x30 条数}，每条 16 字节 {u32 动作号, +8 指针}，
// 动作号 = main+0x5b23f0(单位, &类型, &类号, &模式, main+0x1d5a900, 0) 改写后的「模式」；[[条目+8]+0x18] 那块的 +8 / +0xc / +0x10 / +0x14 = 这个动作的图形范围 左 / 右 / 上 / 下（经典像素，相对单位脚底），游戏比的时候四周各放 16。
// The game's own "does the point fall on the unit" (main+0x2996b0, used by the keyboard-and-mouse version to select units with the mouse; nobody calls it in this version, so read the same data the way it does): unit +0x78 -> {+0x28 entry array, +0x30 count}, each entry 16 bytes {u32 action number, +8 pointer},
// action number = the "mode" as rewritten by main+0x5b23f0(unit, &type, &class, &mode, main+0x1d5a900, 0); +8 / +0xc / +0x10 / +0x14 of the block at [[entry+8]+0x18] = the graphic extent of this action left / right / top / bottom (classic pixels, relative to the unit's feet), which the game pads by 16 on every side when comparing.
static int w_sprite_box(uint8_t *b, uint64_t u, int out[4]) {
    uint64_t tb = *(uint64_t *)(u + 0x78); if (!tb) return 0; uint64_t arr = *(uint64_t *)(tb + 0x28), cnt = *(uint64_t *)(tb + 0x30); if (!arr || !cnt || cnt > 4096) return 0;
    uint32_t type = *(uint32_t *)u, cls = *(uint32_t *)(u + 4), mode = (type <= 1 && *(uint64_t *)(u + 0x40)) ? *(uint32_t *)(u + 0x58) : *(uint32_t *)(u + 0xc);
    ((void (*)(uint64_t, uint32_t *, uint32_t *, uint32_t *, uint64_t, int))(b + 0x5b23f0))(u, &type, &cls, &mode, (uint64_t)(uintptr_t)(b + 0x1d5a900), 0);
    for (uint64_t i = 0; i < cnt; i++) { uint64_t e = arr + i * 16; if (*(uint32_t *)e != mode) continue; uint64_t q = *(uint64_t *)(e + 8), r = q ? *(uint64_t *)(q + 0x18) : 0; if (!r) return 0;
        out[0] = *(int32_t *)(r + 8); out[1] = *(int32_t *)(r + 0xc); out[2] = *(int32_t *)(r + 0x10); out[3] = *(int32_t *)(r + 0x14); return out[1] > out[0] && out[3] > out[2] && out[1] - out[0] < 2000 && out[3] - out[2] < 2000; }
    return 0;
}
static uint64_t w_hit(uint8_t *b, float sx, float sy, float min, float *dist, int *by_label) {
    uint64_t pl = w_player(), scene = *(uint64_t *)(b + 0x2e2a698), best = 0; float bd = 1e9f, d; int brank = 2; *by_label = 0; if (!pl || !scene) return 0;
    int RWi = 0, RHi = 0; dm_screen_real(&RWi, &RHi); float RW = RWi > 0 ? RWi : 1, RH = RHi > 0 ? RHi : 1; int cam = w_cam_convert();   // 镜头实验开着：逐个候选换触点（见下） | camera experiment on: convert the touch point per candidate (see below)
    int persp = !cam && dm_cam_on();
    static int dumps; int dump = getenv("DM_WORLD_DUMP") && dumps++ < 3;   // 诊断：前 3 次点按把全部候选及其屏幕矩形打出来 | diagnostics: for the first 3 taps print all candidates and their screen rectangles
    uint64_t L = (uint64_t)(uintptr_t)(b + 0x2f0fee0), rec = *(uint64_t *)L, n = *(uint64_t *)(L + 8);
    // 标签矩形是界面坐标（游戏投影出来的，已被减掉界面矩形的 y），点按是真实画面坐标：比的时候加回
    // Label rectangles are in UI coordinates (projected by the game, with the UI rectangle's y already subtracted), taps are in real picture coordinates: add it back when comparing
    for (uint64_t k = 0; rec && k < n && k < 512; k++) { int32_t *r = (int32_t *)(rec + k * 0x148); if (r[2] <= 0 || r[3] <= 0 || r[4] == -1) continue;
        if (dump) LOG("世界目标：标签 物品 id %d 矩形 (%d,%d %dx%d)", r[4], r[0], r[1], r[2], r[3]);
        if (w_in_rect(sx, sy, r[0] + r[2] / 2.0f, r[1] + dm_ui_proj_dy() + r[3] / 2.0f, (float)r[2], (float)r[3], min, &d) && d < bd) { uint64_t u = w_find(4, (uint32_t)r[4]); if (u && *(uint32_t *)(u + 0xc) == 3) { bd = d; best = u; brank = 0; *by_label = 1; } } }
    for (int t = 0; t <= 5; t++) { if (t == 3) continue;   // 0 = 玩家类：只认尸体（见下） | 0 = player type: only corpses count (see below)
        for (int k = 0; k < 128; k++) for (uint64_t u = *(uint64_t *)(b + 0x2d603a0 + t * 0x400 + k * 8); u; u = *(uint64_t *)(u + 0x158)) {
            if (t == 4 && *(uint32_t *)(u + 0xc) != 3) continue;   // 物品只认在地上的 | items only count when on the ground
            if (!(((int (*)(uint64_t, int, int, int, int))(b + 0x464a70))(u, 0, 0, -1, -1) & 1)) continue;
            if (t == 0 && u == pl) continue;
            int cat = ((int (*)(int, uint64_t, uint64_t))(b + 0x2f7b7c))(0x165, pl, u); if (t == 0 ? cat != 9 : t == 1 ? (cat != 5 && !W_MON_OK(cat)) : t == 2 ? cat != 2 : 0) continue;   // 类别 9 = 死亡状态的玩家单位（0x2f7c28–0x2f7c34：main+0x4667a0 对玩家类判模式 0x11）= 尸体，点它 = 取回装备（游戏点击入口对玩家类发 0x13） | category 9 = a player unit in the dead state (0x2f7c28–0x2f7c34: main+0x4667a0 checks mode 0x11 for the player type) = corpse, tapping it = retrieve equipment (the game's click entry sends 0x13 for the player type)
            float f[2], h[2]; if (!w_project(b, scene, u, f, 0)) continue;
            if (t == 4 || !w_project(b, scene, u, h, 1)) { h[0] = f[0]; h[1] = f[1]; }
            // 越肩镜头、框已在透视画面上（persp）：紧贴镜头近平面的单位，投影会炸成远大于画面的框，点哪都中它（实测沃里夫站在镜头旁：框 12937 × 29403、
            // 框心在画面外，点远处的游侠却跟他说上了话）。框心不在画面里（四周各留 1/4）或比画面高两倍的，玩家不可能是在点它，不算
            // Over-the-shoulder camera with boxes already on the perspective picture (persp): a unit hugging the camera's near plane projects to a box far
            // larger than the picture, and every tap hits it (measured: Warriv standing next to the camera got a 12937 x 29403 box centred off the picture,
            // and tapping a distant rogue started a talk with him). A box whose centre is off the picture (with a quarter of margin on every side) or that is
            // more than twice the picture's height cannot be what the player tapped, so it does not count
            if (persp) { float bx = (f[0] + h[0]) / 2, by = (f[1] + h[1]) / 2; if (bx < -RW / 4 || bx > RW * 1.25f || by < -RH / 4 || by > RH * 1.25f || fabsf(f[1] - h[1]) > 2 * RH) continue; }
            // 镜头实验（dm_cam.c）：触点是透视画面上的点，这个单位的框是原版正交画面上的——把触点搬到「过它脚底的竖直面」那一层，换回原版画面再比；镜头背后的不算
            // Camera experiment (dm_cam.c): the touch point is on the perspective picture while this unit's boxes are on the original orthographic picture -- move the touch point to the vertical plane through its feet, convert back to the original picture, then compare; units behind the camera do not count
            float csx = sx, csy = sy; if (cam) { float ox, oy; if (!dm_cam_tap_for_unit(sx / RW, sy / RH, f[0] / RW, f[1] / RH, &ox, &oy)) continue; csx = ox * RW; csy = oy * RH; }
            float hh = fabsf(f[1] - h[1]), ww = hh * (t == 1 ? 0.44f : 1.0f), cx = (f[0] + h[0]) / 2, cy = (f[1] + h[1]) / 2, dhw = 0, dhv = 0, sbx = 0, sby = 0, sbw = 0, sbh = 0;
            if (t == 0) { cx = f[0]; cy = f[1]; ww = hh; hh = 0; }   // 尸体躺在地上：框取以脚底点为中心、宽 = 身长的一条（高不足补到 min），不用「脚底—头顶」那根竖柱（截图核过，竖柱在尸体上方） | a corpse lies on the ground: the box is a strip centered on the feet point, width = body length (height padded up to min), not the "feet-to-head" vertical column (checked by screenshot, the column is above the corpse)
            if (t == 2) {   // 物件：再并上它的占地范围——贴地的（小站法阵）「身高」很小，光靠身体框只盖住上半截（真实触点：点法阵中心没有目标）。 | object: also union in its footprint -- ground-hugging ones (the waypoint circle) have a very small "height", and the body box alone covers only the upper half (real touch point: tapping the center of the circle found no target).
                // 占地 = 物件表记录（[[单位+0x10]]）+0x90 / +0x94 的 SizeX / SizeY（子格；小站 5×5、箱子 1×1，运行时读出），四周各放 1 格（美术比碰撞大一圈，截图量的），
                // 按等距投影成以脚底为中心的屏幕菱形（地砖是菱形，矩形的四角是空地）：半宽 = n×16×k、半高 = n×8×k，k = 每「经典像素」的屏幕像素 = 画面高 × 0.00206（900 高时 1.85，两个已知单位的屏幕间距量的）
                // Footprint = SizeX / SizeY at +0x90 / +0x94 of the objects table record ([[unit+0x10]]) (sub-cells; waypoint 5x5, chest 1x1, read at run time), padded by 1 cell on every side (the art is one ring larger than the collision, measured from screenshots),
                // projected isometrically into a screen diamond centered on the feet (floor tiles are diamonds; the four corners of a rectangle are empty ground): half width = n*16*k, half height = n*8*k, k = screen pixels per "classic pixel" = picture height * 0.00206 (1.85 at height 900, measured from the screen distance between two known units)
                uint64_t dd = *(uint64_t *)(u + 0x10), rec = dd ? *(uint64_t *)dd : 0; uint32_t szx = rec ? *(uint32_t *)(rec + 0x90) : 0, szy = rec ? *(uint32_t *)(rec + 0x94) : 0;
                if (szx >= 1 && szx <= 32 && szy >= 1 && szy <= 32) { float n = (szx + szy) / 2.0f + 2, k = min / 0.10f * 0.00206f; dhw = n * 16 * k; dhv = n * 8 * k; }
                // 再并上游戏自己的选择范围（键鼠版鼠标选物件用的数据，读 main+0x464a70 / 0x2996b0）：
                // ① 物件表的选择框：记录 +0x14c / +0x150 / +0x154 / +0x158 = Left / Top / Width / Height（经典像素，相对脚底；红门 342：-40,-80,80,80；回城门：-40,-100,80,110）；
                // ② 表里没给选择框的（四个数全 0，如第四幕去第五幕的红门 566、小站、箱子）：游戏改用这个物件当前动作的图形范围（w_sprite_box），四周放 16。图形范围含光晕 / 影子，左右会很宽
                //（566：左 -136 右 60），这里左右收到脚底两侧各 40 经典像素以内、上边封到 110（表里几扇传送门的选择框就是这个宽度与高度），免得点门旁、门后远处的空地也算点门；下边照用。
                // 起因：566 的「头顶」投影离脚底很近，身体框只盖住门脚，点图案正中没有目标
                // Then union in the game's own selection extent (the data the keyboard-and-mouse version uses to select objects with the mouse, read by main+0x464a70 / 0x2996b0):
                // (1) the selection box in the objects table: record +0x14c / +0x150 / +0x154 / +0x158 = Left / Top / Width / Height (classic pixels, relative to the feet; red portal 342: -40,-80,80,80; town portal: -40,-100,80,110);
                // (2) for those the table gives no selection box (all four numbers 0, e.g. the act 4 to act 5 red portal 566, waypoints, chests): the game uses the graphic extent of the object's current action instead (w_sprite_box), padded by 16 all around. The graphic extent includes glow / shadow and can be very wide left to right
                // (566: left -136, right 60), so here left and right are clamped to within 40 classic pixels on each side of the feet and the top is capped at 110 (the selection boxes of the several portals in the table have exactly this width and height), so that tapping empty ground beside or far behind the door does not count as tapping the door; the bottom is used as is.
                // Cause: the "head" projection of 566 is very close to the feet, the body box covers only the foot of the door, and tapping the middle of the artwork found no target
                if (rec) { int32_t *ri = (int32_t *)rec; float k = min / 0.10f * 0.00206f; int sb[4];
                    if (ri[85] > 0 && ri[86] > 0 && ri[85] <= 400 && ri[86] <= 400 && abs(ri[83]) <= 400 && abs(ri[84]) <= 400) { sbw = ri[85] * k; sbh = ri[86] * k; sbx = f[0] + ri[83] * k + sbw / 2; sby = f[1] + ri[84] * k + sbh / 2; }
                    else if (w_sprite_box(b, u, sb)) { int l = sb[0] - 16, r = sb[1] + 16, tp = sb[2] - 16, bt = sb[3] + 16; if (l < -40) l = -40; if (r > 40) r = 40; if (tp < -110) tp = -110;
                        if (r > l) { sbw = (r - l) * k; sbh = (bt - tp) * k; sbx = f[0] + (l + r) / 2.0f * k; sby = f[1] + (tp + bt) / 2.0f * k; } } } }
            if (dump) { int sb[4] = {0, 0, 0, 0}; int okb = w_sprite_box(b, u, sb); LOG("世界目标：  [诊断] 类型 %d cls %u 游戏图形范围 %s 左 %d 右 %d 上 %d 下 %d（经典像素）", t, *(uint32_t *)(u + 4), okb ? "有" : "没有", sb[0], sb[1], sb[2], sb[3]); }
            if (dump) LOG("世界目标：候选 类型 %d cls %u id %u 身体框 中心 (%.0f,%.0f) %.0fx%.0f（不足 %.0f 的边补到 %.0f）；占地菱形 中心 (%.0f,%.0f) 半宽 %.0f 半高 %.0f；选择框 中心 (%.0f,%.0f) %.0fx%.0f", t, *(uint32_t *)(u + 4), *(uint32_t *)(u + 8), cx, cy, ww, hh, min, min, f[0], f[1], dhw, dhv, sbx, sby, sbw, sbh);
            int in = w_in_rect(csx, csy, cx, cy, ww, hh, min, &d);
            if (!in && dhw > 0) { float ax = fabsf(csx - f[0]), ay = fabsf(csy - f[1]); if (ax / dhw + ay / dhv <= 1) { in = 1; d = sqrtf(ax * ax + ay * ay); } }
            int rank = 0; if (!in && sbw > 0) { in = w_in_rect(csx, csy, sbx, sby, sbw, sbh, min, &d); rank = 1; }   // 只靠选择框命中的排在身体框 / 菱形 / 标签命中的之后（别让门的大框压过站在门旁、被自己身体框点中的 NPC） | hits by selection box alone rank after hits by body box / diamond / label (do not let a door's large box beat an NPC standing by the door who was hit through his own body box)
            if (in && (rank < brank || (rank == brank && d < bd))) { bd = d; best = u; brank = rank; *by_label = 0; } } }
    *dist = best ? bd : 0; return best;
}
// 任意线程：排一笔点按（出图画面归一化坐标），主线程取手柄处执行。后一笔覆盖前一笔
// Any thread: queue one tap (coordinates normalized to the rendered picture), executed by the main thread at the controller-poll point. A later one overwrites the earlier one
static _Atomic uint64_t g_tap; static _Atomic unsigned g_tap_seq, g_tap_done; static _Atomic int g_tap_res, g_has_target;
unsigned dm_world_tap(float nx, float ny) { if (nx < 0) nx = 0; if (nx > 1) nx = 1; if (ny < 0) ny = 0; if (ny > 1) ny = 1;
    unsigned seq = atomic_fetch_add(&g_tap_seq, 1) + 1; atomic_store(&g_tap, (uint64_t)seq << 32 | (uint64_t)(uint32_t)(nx * 65535) << 16 | (uint32_t)(ny * 65535)); return seq; }
int dm_world_tap_result(unsigned seq) { return atomic_load(&g_tap_done) == seq ? atomic_load(&g_tap_res) : -1; }   // -1 还没执行；0 没有目标（什么都没发）；1 物品 2 NPC 3 物件 4 出口 | -1 not executed yet; 0 no target (nothing sent); 1 item 2 NPC 3 object 4 exit
int dm_world_has_target(void) { return atomic_load(&g_has_target); }   // 按主键会去交互的目标存在（游戏手柄版自己的选择，main+0x2f7a0c）；任意线程读 | a target that the primary button would interact with exists (the game's own controller-version selection, main+0x2f7a0c); read from any thread
#if TARGET_OS_OSX
static int w_hostile(uint8_t *b, uint64_t u) { if (*(uint32_t *)u != 1) return 0; uint64_t pl = w_player(); int c = pl ? ((int (*)(int, uint64_t, uint64_t))(b + 0x2f7b7c))(0x165, pl, u) : 0; return c == 1 || c == 0xa; }
static uint64_t g_mouse_attack_at;
static void w_attack(uint64_t u, const char *how) { g_mouse_attack_at = w_now_ns(); if (g_go.on) g_go.on = 0; int r = w_interact(u); static int lg; if (lg++ < 300) LOG("鼠标攻击：%s 类型 1 cls %u id %u → 点击入口 %d", how, *(uint32_t *)(u + 4), *(uint32_t *)(u + 8), r); }
static void w_walk_step(uint8_t *b) {
    uint64_t t = atomic_exchange(&g_walk, 0); if (!t) return; uint64_t dm_stick_last_ns(void); uint64_t pl = w_player(), scene = *(uint64_t *)(b + 0x2e2a698); int W = 0, H = 0, px = 0, py = 0; float f[2];
    dm_screen_real(&W, &H); if (!pl || !scene || W <= 0 || H <= 0 || !w_unit_xy(pl, &px, &py) || !w_project(b, scene, pl, f, 0)) return;
    { float d0 = 0; int lab0 = 0; g_hit_mon = 1; uint64_t mu = w_hit(b, ((t >> 16) & 0xffff) / 65535.0f * W, (t & 0xffff) / 65535.0f * H, H * 0.10f, &d0, &lab0); g_hit_mon = 0;
      if (mu && w_hostile(b, mu)) { if (w_now_ns() - g_gnd_stick_at >= 300000000ull && w_now_ns() - g_mouse_attack_at >= 200000000ull) w_attack(mu, "按住"); return; } }   // 按住时光标下是敌对怪：再打一下（摇杆 / W A S D 刚动过就让给它） | while held with a hostile monster under the pointer: hit once more (yielding to the stick / W A S D if they have just moved)
    if (g_go.on && g_go.type != W_GROUND) return;   // 正走向某个目标去交互：不打断 | walking towards some target to interact: do not interrupt
    if (w_now_ns() - g_gnd_stick_at < 300000000ull) return;   // 摇杆 / W A S D 0.3 秒内动过：让给它 | the stick / W A S D moved within 0.3 s: yield to it
    float sx = ((t >> 16) & 0xffff) / 65535.0f * W, sy = (t & 0xffff) / 65535.0f * H, k = H * 0.00206f, a = (sx - f[0]) / k / 16, c = (sy - f[1]) / k / 8;
    int x = px + (int)lroundf((a + c) / 2), y = py + (int)lroundf((c - a) / 2);
    if (getenv("DM_TEST_WALK_CAL")) { static int n; if (n++ < 3)   // 测试：把画面里各单位的脚底屏幕点按同一条公式反算成格，和它真实所在的格比（量换算误差） | test: back-compute the foot screen points of units on screen into tiles with the same formula and compare with the tiles they are really on (to measure the conversion error)
        for (int ty = 1; ty <= 2; ty++) for (int kk = 0; kk < 128; kk++) for (uint64_t u = *(uint64_t *)(b + 0x2d603a0 + ty * 0x400 + kk * 8); u; u = *(uint64_t *)(u + 0x158)) { float g[2]; int ux, uy;
            if (!w_unit_xy(u, &ux, &uy) || !w_project(b, scene, u, g, 0) || g[0] < 0 || g[1] < 0 || g[0] > W || g[1] > H) continue; float a2 = (g[0] - f[0]) / k / 16, c2 = (g[1] - f[1]) / k / 8;
            LOG("点地面走：[校] 类型 %d cls %u 脚底 (%.0f,%.0f) 真格 %d,%d 反算 %d,%d 差 %d,%d（离人 %d,%d 格）", ty, *(uint32_t *)(u + 4), g[0], g[1], ux, uy, px + (int)lroundf((a2 + c2) / 2), py + (int)lroundf((c2 - a2) / 2), px + (int)lroundf((a2 + c2) / 2) - ux, py + (int)lroundf((c2 - a2) / 2) - uy, ux - px, uy - py); } }
    if (g_go.on && abs(x - g_gnd_x) <= 1 && abs(y - g_gnd_y) <= 1) return;   // 按住不动时重发的同一格：不重起 | the same tile re-sent while holding still: do not restart
    static int lg; if (lg++ < 400) LOG("点地面走：点 (%.0f,%.0f)，人脚底在 (%.0f,%.0f) → 格 %d,%d（玩家 %d,%d）", sx, sy, f[0], f[1], x, y, px, py);
    g_go.type = W_GROUND; g_go.id = 0; g_go.nearest = 0; g_go.until_ns = w_now_ns() + 15000000000ull; g_go.stick = dm_stick_last_ns(); g_go.next_fr = 0; g_go.still_fr = 0;
    g_go.n = g_go.i = g_go.replans = g_go.empty = g_go.direct = g_go.alt = g_go.alt_on = 0; g_go.stop_at = 0; g_gnd_x = x; g_gnd_y = y; g_go.on = 1;
}
#endif
#if TARGET_OS_OSX
// ---- Mac 鼠标：点怪 = 攻击 ----
// 发令走游戏自己的点击入口（同 w_interact：手柄主键与键鼠左键共用的 main+0x4ace20，带上这只怪当指定目标）：近战它自己走过去打，主键上绑的是技能就对它放那个技能，不另写攻击逻辑。
// 按住不放时 touch_mac.c 每 30 Hz 拍更新「朝光标走」（dm_world_walk）：敌对怪改成攻击（追着打），重复攻击仍至少间隔 0.2 秒，不是怪才走路。
// ---- Mac mouse: click a monster = attack ----
// Issued through the game's own click entry (same as w_interact: main+0x4ace20, shared by the controller main button and the keyboard/mouse left button, with this monster as the explicit target): melee walks over and hits; if the main button is bound to a skill, that skill is cast on it; no separate attack logic.
// While held, touch_mac.c updates "walk toward the cursor" (dm_world_walk) every 30 Hz tick: a hostile monster turns into an attack (chasing it), repeated attacks are still at least 0.2 s apart, and only non-monsters mean walking.
static _Atomic int g_tap_mouse;
unsigned dm_world_click(float nx, float ny) { atomic_store(&g_tap_mouse, 1); return dm_world_tap(nx, ny); }   // 任意线程：鼠标左键点世界（比触屏的点按多认敌对怪） | any thread: left mouse click on the world (recognises hostile monsters in addition to what a touch tap does)
// 世界泵早于鼠标 hook：消费前也要取消暂停、面板或离局期间排队的鼠标指令，触屏/脚本点按不动。
// The world pump precedes the mouse hook: also cancel queued mouse commands before consumption during pause, panels or game exit; leave touch/script taps alone.
static void w_mouse_cancel_pending(void) {
    if (dm_touch_host_world_active()) return;
    if (atomic_exchange(&g_tap_mouse, 0)) atomic_store(&g_tap, 0);
    atomic_store(&g_walk, 0);
}
#endif
static void w_tap_step(uint8_t *b) {
    uint64_t t = atomic_exchange(&g_tap, 0); if (!t) return; unsigned seq = (unsigned)(t >> 32); int res = 0;
    int W = 0, H = 0; dm_screen_real(&W, &H);   // 命中框按真实画面算，不能用游戏的「屏幕宽高」全局量（界面改成 16:9 后它的高是界面高，dm_uiscreen.c） | hit boxes are computed against the real picture, not the game's "screen width/height" globals (after the UI was changed to 16:9 its height is the UI height, dm_uiscreen.c)
    float sx = ((t >> 16) & 0xffff) / 65535.0f * W, sy = (t & 0xffff) / 65535.0f * H, d = 0; int lab = 0;
#if TARGET_OS_OSX
    g_hit_mon = atomic_exchange(&g_tap_mouse, 0);
#endif
    uint64_t u = W > 0 && H > 0 ? w_hit(b, sx, sy, H * 0.10f, &d, &lab) : 0;
#if TARGET_OS_OSX
    g_hit_mon = 0;
    if (u && w_hostile(b, u)) { w_attack(u, "点"); u = 0; res = 6; }   // 鼠标点到敌对怪：攻击（见 w_attack），不走「走过去交互」 | the mouse clicked a hostile monster: attack (see w_attack), not "walk over and interact"
#endif
    if (u) { uint32_t ty = *(uint32_t *)u; res = ty == 4 ? 1 : ty == 1 ? 2 : ty == 2 ? 3 : ty == 0 ? 5 : 4; w_go(u); }
    static int logged; if (logged++ < 200) { if (u) LOG("世界目标：点 (%.0f,%.0f) → 类型 %u cls %u id %u（%s，离框中心 %.0f 像素）", sx, sy, *(uint32_t *)u, *(uint32_t *)(u + 4), *(uint32_t *)(u + 8), lab ? "名字标签" : "命中框", d); else if (res != 6) LOG("世界目标：点 (%.0f,%.0f) → 没有目标，不发令", sx, sy); }
    atomic_store(&g_tap_res, res); atomic_store(&g_tap_done, seq);
}
// 测试量具（触点脚本 tap_unit）：盯住某一类单位，主线程每帧把它身体框中心的屏幕位置（出图画面归一化）发布出来，脚本线程读了去落真实触点。
// 会走动的 NPC 每局站位不同，固定屏幕坐标的脚本点不中（连走两遍恰西 0/2）。
// Test probe (touch point script tap_unit): watch one kind of unit; every frame the main thread publishes the screen position of its body box center (normalized to the rendered picture), and the script thread reads it to drop a real touch point.
// Walking NPCs stand in different places every run, so scripts with fixed screen coordinates miss them (Charsi 0/2 in two consecutive passes).
static _Atomic uint32_t g_watch; static _Atomic uint64_t g_watch_pos;
void dm_world_watch(int type, uint32_t cls) { atomic_store(&g_watch_pos, 0); atomic_store(&g_watch, type < 0 ? 0 : 0x80000000u | (uint32_t)type << 24 | (cls & 0xffffff)); }
int dm_world_watch_pos(float *nx, float *ny) { uint64_t v = atomic_load(&g_watch_pos); if (!v) return 0; *nx = ((v >> 16) & 0xffff) / 65535.0f; *ny = (v & 0xffff) / 65535.0f; return 1; }
static void w_watch_step(uint8_t *b) {
    uint32_t w = atomic_load(&g_watch); if (!w) return; int t = (w >> 24) & 0x7f; uint32_t cls = w & 0xffffff; uint64_t scene = *(uint64_t *)(b + 0x2e2a698), v = 0;
    int W = 0, H = 0; dm_screen_real(&W, &H);
    for (int k = 0; scene && W > 0 && H > 0 && t <= 5 && k < 128 && !v; k++) for (uint64_t u = *(uint64_t *)(b + 0x2d603a0 + t * 0x400 + k * 8); u; u = *(uint64_t *)(u + 0x158)) {
#if TARGET_OS_OSX
        if (cls == 0xfffffe) { if (!(((int (*)(uint64_t, int, int, int, int))(b + 0x464a70))(u, 0, 0, -1, -1) & 1) || !w_hostile(b, u)) continue; } else   // 类号 0xfffffe（Mac 合成鼠标脚本的 unit 1,hostile）= 敌对且可选中的怪 | class 0xfffffe (unit 1,hostile in the Mac synthetic mouse script) = hostile and selectable monster
#endif
        if (cls != 0xffffff ? *(uint32_t *)(u + 4) != cls : (t == 4 && *(uint32_t *)(u + 0xc) != 3)) continue;   // 类号 0xffffff（开关里写 any）= 这一类里任意一个；物品只认在地上的 | class number 0xffffff (written as any in the switch) = any one of this type; items only count when on the ground
        float f[2], h[2]; if (!w_project(b, scene, u, f, 0)) continue;
        if (!w_project(b, scene, u, h, 1)) { h[0] = f[0]; h[1] = f[1]; }
        float cx = (f[0] + h[0]) / 2 / W, cy = (f[1] + h[1]) / 2 / H;
        if (w_cam_convert()) { float ox, oy; if (!dm_cam_unit_to_screen(f[0] / W, f[1] / H, h[0] / W, h[1] / H, &ox, &oy)) continue; cx = ox; cy = oy; }   // 镜头实验：落到玩家在透视画面上真看见它的地方；在镜头背后的不算 | camera experiment: land where the player actually sees it on the perspective picture; units behind the camera do not count
        if (cx < 0 || cx > 1 || cy < 0 || cy > 1) continue;
        v = 1ull << 32 | (uint64_t)(uint32_t)(cx * 65535) << 16 | (uint32_t)(cy * 65535); break; }
    atomic_store(&g_watch_pos, v);
}
// 测试开关（造现场用，走不到的地方先走过去）：DM_TEST_WORLD_XY="<拍>:<x>,<y>[;…]" 到拍后按格坐标走过去（同一套路线规划，逐段发游戏的「跑到点」；到 2 格内或 12 秒没到就换下一条）；
// DM_TEST_WORLD_LIST="<拍>[,<拍>…]" 到拍把当前载入的 NPC / 怪（类型 1）与物件（类型 2）的类号和格坐标列出来（各最多 60 个）。
// Test switch (for setting up a test case, walk first to places that cannot be reached otherwise): DM_TEST_WORLD_XY="<tick>:<x>,<y>[;...]" after the tick walk there by cell coordinates (same route planning, sending the game's "run to point" segment by segment; move on to the next entry once within 2 cells or after 12 seconds without arriving);
// DM_TEST_WORLD_LIST="<tick>[,<tick>...]" at the tick list the class numbers and cell coordinates of the currently loaded NPCs / monsters (type 1) and objects (type 2) (at most 60 each).
static void w_test_xy_step(void) {
    static struct { uint32_t at; int x, y, st; uint64_t until; } q[12]; static int n = -1, cur = -1; static uint32_t lt[8]; static int nl = -1; uint8_t *b = wb(); uint64_t pl = w_player(); if (!b || !pl) return;
    if (n < 0) { n = 0; const char *e = getenv("DM_TEST_WORLD_XY"); char buf[400]; if (e) { strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ";", &sv); t && n < 12; t = strtok_r(NULL, ";", &sv)) if (sscanf(t, "%u:%d,%d", &q[n].at, &q[n].x, &q[n].y) == 3) n++; } }
    if (nl < 0) { nl = 0; const char *e = getenv("DM_TEST_WORLD_LIST"); char buf[100]; if (e) { strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t && nl < 8; t = strtok_r(NULL, ",", &sv)) lt[nl++] = (uint32_t)atoi(t); } }
    uint32_t dm_cur_tick(void); uint32_t tk = dm_cur_tick(); int px = 0, py = 0; w_unit_xy(pl, &px, &py);
    for (int i = 0; i < nl; i++) if (lt[i] && tk >= lt[i]) { lt[i] = 0; LOG("单位列表（拍 %u，玩家 %d,%d）：", tk, px, py);
        { const char *e = getenv("DM_TEST_OBJREC"); char buf[80]; if (e) { strlcpy(buf, e, sizeof buf); uint64_t t = *(uint64_t *)(b + 0x2d64a18 + (uint64_t)*(uint8_t *)(pl + 0x1bd) * 16);
            for (char *sv, *tkn = strtok_r(buf, ",", &sv); tkn && t; tkn = strtok_r(NULL, ",", &sv)) { uint32_t c = (uint32_t)atoi(tkn); if (c >= *(uint64_t *)(t + 0x1550)) continue; int32_t *r = (int32_t *)(*(uint64_t *)(t + 0x1548) + (uint64_t)c * 0x168); char o[1400]; int k = 0;
                for (int q = 0; q < 0x168 / 4; q++) k += snprintf(o + k, sizeof o - k, " %d", r[q]); if (k >= (int)sizeof o) k = (int)sizeof o - 1; LOG("物件表记录 cls %u（每格 4 字节，十进制）：%s", c, o); } } }
        for (int gt = 1; gt <= 5; gt += gt == 2 ? 3 : 1) { char o[1600]; int c2 = 0, cnt = 0; o[0] = 0; for (int k = 0; k < 128; k++) for (uint64_t c = *(uint64_t *)(b + 0x2d603a0 + gt * 0x400 + k * 8); c && cnt < 60; c = *(uint64_t *)(c + 0x158)) { int x = 0, y = 0; w_unit_xy(c, &x, &y); int sel = ((int (*)(uint64_t, int, int, int, int))(b + 0x464a70))(c, 0, 0, -1, -1) & 1, cat = ((int (*)(int, uint64_t, uint64_t))(b + 0x2f7b7c))(0x165, pl, c);
            c2 += snprintf(o + c2, sizeof o - c2, " c%u/模式%u%s(%d,%d)", *(uint32_t *)(c + 4), *(uint32_t *)(c + 0xc), !sel ? "/不可选" : cat == 2 ? "/物" : cat == 4 ? "/出口" : cat == 5 ? "/人" : cat ? "/别" : "/无类", x, y); if (c2 >= (int)sizeof o) c2 = (int)sizeof o - 1; cnt++; } LOG("  类型 %d：%s", gt, o); } }
    if (cur < 0) { for (int i = 0; i < n; i++) if (!q[i].st && tk >= q[i].at) { cur = i; q[i].st = 1; q[i].until = w_now_ns() + 12000000000ull; g_go.on = 0; g_go.n = g_go.i = 0; LOG("按坐标走：→ %d,%d（从 %d,%d）", q[i].x, q[i].y, px, py); break; } if (cur < 0) return; }
    int dx = abs(q[cur].x - px), dy = abs(q[cur].y - py); static uint64_t nf;
    if ((dx <= 2 && dy <= 2) || w_now_ns() > q[cur].until) { LOG("按坐标走：%s，停在 %d,%d（目标 %d,%d）", dx <= 2 && dy <= 2 ? "到了" : "12 秒没到", px, py, q[cur].x, q[cur].y); q[cur].st = 2; cur = -1; return; }
    while (g_go.i < g_go.n && abs((int)g_go.node[g_go.i][0] - px) <= 1 && abs((int)g_go.node[g_go.i][1] - py) <= 1) { g_go.i++; nf = 0; }
    uint64_t fr = dm_guest_frame(); if (fr < nf) return; nf = fr + 20;
    if (g_go.i >= g_go.n && !w_plan(b, pl, q[cur].x, q[cur].y)) return;
    atomic_store(&g_dm_move_allow_until_ns, w_now_ns() + 500000000ull); ((void (*)(int, uint64_t, int, int))(b + 0x4ad870))(3, pl, g_go.node[g_go.i][0], g_go.node[g_go.i][1]);
}
// 测试开关（造现场用）DM_TEST_WORLD_SEEK="<拍>:<类型>:c<类号>[:L<关卡号>][;…]"：到拍后（写了关卡号就等人到了那个关卡才开始）在当前关卡里找这个单位，找到就走过去交互（同 DM_TEST_WORLD_GO）。
// 没载入的房间没有物件 / 出口的数据（游戏载入房间时才生成），目标离得远时按类号指不到，所以要让人先过去：服务端一侧（diag.c seek_tp_step）用游戏自己的传送一间房一间房地跳
// （门、怪、墙都不挡），这里只等目标出现在单位表里 → 请那边把人传到目标旁 → 等 90 帧 → 走过去交互。几条按顺序做，可跨关卡（例：峡谷里找通往真墓的出口砖，进墓后找法杖底座）。
// 出口砖是类型 5，类号 = 关卡表里通往目标关卡的那个出口编号（法师峡谷 → 66..72 是 38..44）。DM_TEST_SEEK_BIAS=x,y：先往离这个点近的房间跳（已知目标大概在哪时用）。
// DM_TEST_LEVEL_INFO="<拍>[,…]"：打印当前关卡号、房间数与范围、真墓关卡号。关卡结构只读游戏内存：玩家所在房间 +0x18 = 关卡房间，其 +0x90 = 关卡；关卡 +0x10 = 首个关卡房间、关卡房间 +0x48 = 下一个（含没载入的），
// +0x60 / +0x64 / +0x68 / +0x6c = x / y / 宽 / 高（×5 = 走位格坐标），+0x58 = 已载入时的房间；关卡 +0x1f8 = 关卡号，关卡 +0x1c8 = 幕级对象，其 +0x120 = 本局藏法杖底座的真墓关卡号（第二幕）。
// Test switch (for setting up a test case) DM_TEST_WORLD_SEEK="<tick>:<type>:c<class>[:L<level>][;...]": after the tick (if a level id is given, it starts only once the player is in that level) look for this unit in the current level, and when found walk over and interact (same as DM_TEST_WORLD_GO).
// Rooms that are not loaded have no object / exit data (the game generates it only when loading the room), so a distant target cannot be addressed by class number and the player must get there first: the server side (diag.c seek_tp_step) jumps room by room with the game's own teleport
// (doors, monsters and walls do not block), and here we only wait for the target to appear in the unit table -> ask that side to teleport the player next to the target -> wait 90 frames -> walk over and interact. Several entries are done in order and may cross levels (example: in the canyon find the exit tile leading to the true tomb, and after entering the tomb find the staff holder).
// An exit tile is type 5, class number = the exit number in the levels table leading to the target level (Canyon of the Magi -> 66..72 is 38..44). DM_TEST_SEEK_BIAS=x,y: jump first to rooms close to this point (used when the target's rough location is known).
// DM_TEST_LEVEL_INFO="<tick>[,...]": print the current level id, room count and extent, and the true tomb level id. Level structure is only read from game memory: the player's room +0x18 = level room, its +0x90 = level; level +0x10 = first level room, level room +0x48 = next one (including unloaded ones),
// +0x60 / +0x64 / +0x68 / +0x6c = x / y / width / height (x5 = walk cell coordinates), +0x58 = the room when loaded; level +0x1f8 = level id, level +0x1c8 = act-level object, its +0x120 = level id of the true tomb hiding the staff holder in this run (act 2).
static void w_test_seek_step(void) {
    extern _Atomic int g_dm_seek_tp; extern int g_dm_seek_tp_x, g_dm_seek_tp_y;
    static struct { uint32_t at, type, cls, lvl; int st; } q[8]; static int n = -1, cur = -1; static uint32_t it[6]; static int ni = -1; static uint64_t lv0, settle;
    uint8_t *b = wb(); uint64_t pl = w_player(); if (!b || !pl) return; uint32_t dm_cur_tick(void); uint32_t tk = dm_cur_tick();
    if (n < 0) { n = 0; const char *e = getenv("DM_TEST_WORLD_SEEK"); char buf[300]; if (e) { strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ";", &sv); t && n < 8; t = strtok_r(NULL, ";", &sv)) { q[n].lvl = 0; q[n].st = 0; if (sscanf(t, "%u:%u:c%u:L%u", &q[n].at, &q[n].type, &q[n].cls, &q[n].lvl) >= 3 && q[n].type <= 5) n++; } } }
    if (ni < 0) { ni = 0; const char *e = getenv("DM_TEST_LEVEL_INFO"); char buf[80]; if (e) { strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t && ni < 6; t = strtok_r(NULL, ",", &sv)) it[ni++] = (uint32_t)atoi(t); } }
    if (!n && !ni) return;
    uint64_t pp = *(uint64_t *)(pl + 0x38), r1 = pp ? *(uint64_t *)(pp + 0x20) : 0, r2 = r1 ? *(uint64_t *)(r1 + 0x18) : 0, lv = r2 ? *(uint64_t *)(r2 + 0x90) : 0; int px = 0, py = 0; if (!lv || !w_unit_xy(pl, &px, &py)) return;
    for (int i = 0; i < ni; i++) if (it[i] && tk >= it[i]) { it[i] = 0; int cnt = 0, ld = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = 0, y1 = 0;
        for (uint64_t r = *(uint64_t *)(lv + 0x10); r && cnt < 2000; r = *(uint64_t *)(r + 0x48)) { int x = *(int *)(r + 0x60) * 5, y = *(int *)(r + 0x64) * 5, w = *(int *)(r + 0x68) * 5, h = *(int *)(r + 0x6c) * 5; cnt++; ld += *(uint64_t *)(r + 0x58) != 0; if (x < x0) x0 = x; if (y < y0) y0 = y; if (x + w > x1) x1 = x + w; if (y + h > y1) y1 = y + h; }
        uint64_t am = *(uint64_t *)(lv + 0x1c8); LOG("关卡信息（拍 %u）：关卡号 %u，关卡房间 %d 间（已载入 %d），范围 (%d,%d)–(%d,%d)，玩家 %d,%d；真墓关卡号 %u", tk, *(uint32_t *)(lv + 0x1f8), cnt, ld, x0, y0, x1, y1, px, py, am ? *(uint32_t *)(am + 0x120) : 0); }
    if (cur < 0) { for (int i = 0; i < n; i++) if (!q[i].st && tk >= q[i].at && (!q[i].lvl || q[i].lvl == *(uint32_t *)(lv + 0x1f8))) { cur = i; q[i].st = 1; lv0 = lv; settle = 0; atomic_store(&g_dm_seek_tp, 1); LOG("探路：在关卡 %u 里找 类型 %u 类号 %u", *(uint32_t *)(lv + 0x1f8), q[i].type, q[i].cls); break; } if (cur < 0) return; }
    if (lv != lv0) { LOG("探路：关卡变了（%u），这一条结束", *(uint32_t *)(lv + 0x1f8)); q[cur].st = 2; cur = -1; atomic_store(&g_dm_seek_tp, 0); return; }
    for (int k = 0; k < 128; k++) for (uint64_t c = *(uint64_t *)(b + 0x2d603a0 + q[cur].type * 0x400 + k * 8); c; c = *(uint64_t *)(c + 0x158)) if (*(uint32_t *)(c + 4) == q[cur].cls) { int x = 0, y = 0; w_unit_xy(c, &x, &y);
        { uint32_t ty = *(uint32_t *)c; uint64_t cp = *(uint64_t *)(c + 0x38), cr = cp ? *(uint64_t *)(cp + (ty == 2 || ty == 4 || ty == 5 ? 0 : 0x20)) : 0, cd = cr ? *(uint64_t *)(cr + 0x18) : 0; if (cd && *(uint64_t *)(cd + 0x90) != lv) continue; }   // 只认本关卡里的：相邻关卡已载入的房间里也会有同类号的出口砖（要塞二层找 82 时看见的是一层那块） | only those in this level count: already loaded rooms of adjacent levels can also contain exit tiles with the same class number (looking for 82 on Keep level 2 it saw the one on level 1)
        int s3 = atomic_load(&g_dm_seek_tp); if (s3 == 3) return;
        if (s3 == 1) { g_dm_seek_tp_x = x; g_dm_seek_tp_y = y; atomic_store(&g_dm_seek_tp, 3); LOG("探路：看见 类型 %u 类号 %u 在 %d,%d（玩家 %d,%d）→ 先传到它旁边", q[cur].type, q[cur].cls, x, y, px, py); return; }
        if (s3 == 2) { if (!settle) settle = dm_guest_frame() + 90; if (dm_guest_frame() < settle) return; }   // 传到后等 90 帧再交互：紧跟着传送就点出口砖，服务端收了交互包却不换关卡 | after arriving by teleport wait 90 frames before interacting: tapping the exit tile right after the teleport, the server accepts the interact packet but does not change level
        atomic_store(&g_dm_seek_tp, 0); { const char *na = getenv("DM_TEST_SEEK_NOACT"); int hit = 0; for (const char *t = na; t && *t; ) { if ((uint32_t)atoi(t) == q[cur].cls) hit = 1; t = strchr(t, ','); if (t) t++; }   /* 可写多个类号，逗号分隔（75,558） | Several class numbers may be given, comma separated (75,558) */
          if (hit) { LOG("探路：找到了 类型 %u 类号 %u 在 %d,%d（玩家 %d,%d），DM_TEST_SEEK_NOACT：只到旁边不交互", q[cur].type, q[cur].cls, x, y, px, py); q[cur].st = 2; cur = -1; return; } }
        LOG("探路：找到了 类型 %u 类号 %u 在 %d,%d（玩家 %d,%d）→ 走过去交互", q[cur].type, q[cur].cls, x, y, px, py); w_go(c); q[cur].st = 2; cur = -1; return; }
    if (!atomic_load(&g_dm_seek_tp)) { LOG("探路：跳得到的房间都去过了，没找到 类型 %u 类号 %u", q[cur].type, q[cur].cls); q[cur].st = 2; cur = -1; }
}
_Atomic int g_dm_dur_sync_n; uint32_t g_dm_dur_sync[16][2];   // DM_TEST_DUR：服务端一侧设完耐久后，请这里把客户端那份物品的属性 72 也写成同值（{物品 id, 值}） | DM_TEST_DUR: after the server side sets the durability, it asks here to write stat 72 of the client copy of the item to the same value ({item id, value})
// 小地图进图即标出小站与出入口（只标洞口、楼梯、门、传送门这类真实的出入口，加上小站；野外图之间走过去的边界不标）。自定义页 map_mark 缺省开；DM_MAP_MARK 强制。
// 用的全是原版自己的函数、原版自己的顺序——预设关卡「整关揭示」那段（main+0x553270–0x553298）对每间房做的就是这两步：
//   房间初始化 main+0x52dcc0(数据版本, 关卡房间) → 调地图对象（关卡 +0x1c8）上 +0x838 的「揭示一间房」回调，参数是激活房间（关卡房间 +0x58）。
// 我们只对「有小站 / 有出入口」的房间做：顺着客户端关卡对象的房间链（关卡 +0x10、下一间 +0x48）看房间标志（+0x50）：& 0x30000 = 有小站；& 0xff0 = 带出口位——
// 出口位在地城图上只落在真有出口的那间，在野外图上却几乎每间都有（冰冷之原实测 0x70），所以带出口位的房间先只初始化（约 0.03 ms 一间），出口链表（关卡房间 +0x78）非空才揭示。
// 提前初始化出来的房间不用我们卸：游戏约 37 拍后自己卸掉，人走近时照常重载（实测来回 3 次）；初始化走的是客户端这套对象，不刷怪（服务端怪物数揭示前后不变）。
// 城镇（1 / 40 / 75 / 103 / 109）原版进图就整图揭示，不做。每进一张关卡做一次（等 30 帧让进图自己的载入先走完）。
// The minimap marks waypoints and entrances/exits as soon as a map is entered (only real entrances/exits such as cave mouths, stairs, doors and portals, plus waypoints; borders walked across between wilderness maps are not marked). Custom page map_mark is on by default; DM_MAP_MARK forces it.
// Everything used is the original game's own functions in the original game's own order -- the "reveal whole level" section for preset levels (main+0x553270–0x553298) does exactly these two steps for every room:
//   room initialization main+0x52dcc0(data version, level room) -> call the "reveal one room" callback at +0x838 on the map object (level +0x1c8), with the active room (level room +0x58) as argument.
// We do it only for rooms that "have a waypoint / have an entrance/exit": follow the room chain of the client level object (level +0x10, next room +0x48) and look at the room flags (+0x50): & 0x30000 = has a waypoint; & 0xff0 = carries exit bits --
// on dungeon maps the exit bits fall only on the room that really has an exit, but on wilderness maps almost every room has them (Cold Plains measured 0x70), so rooms carrying exit bits are first only initialized (about 0.03 ms per room) and revealed only if the exit linked list (level room +0x78) is non-empty.
// Rooms initialized early need not be unloaded by us: the game unloads them itself after about 37 ticks and reloads them as usual when the player gets close (measured 3 round trips); the initialization goes through the client-side objects and spawns no monsters (server monster count unchanged before and after the reveal).
// Towns (1 / 40 / 75 / 103 / 109) are revealed whole by the original game on entry, so nothing is done. Done once per level entered (after waiting 30 frames to let the map's own loading finish first).
_Atomic int g_dm_game_ver = -1;
static void w_map_mark_step(void) {
    // 开关：环境变量 DM_MAP_MARK（测试强制，0 关 / 其余开）优先，否则读自定义页 map_mark（运行中即时生效；关掉只是不再标新进的图，已标的迷雾不收回）
    // Switch: the environment variable DM_MAP_MARK (forced for tests, 0 off / anything else on) takes priority, otherwise read map_mark from the custom page (takes effect immediately at run time; turning it off only stops marking newly entered maps, fog already marked is not taken back)
    static int env = -2; static uint64_t lv_seen, lv_done, at; if (env == -2) { const char *e = getenv("DM_MAP_MARK"); env = e ? e[0] != '0' : -1; }
    int dm_custom_get(const char *key); if (!(env >= 0 ? env : dm_custom_get("map_mark"))) return;
    uint8_t *b = wb(); uint64_t pl = b ? w_player() : 0, lv = pl ? w_level(pl) : 0; int ver = atomic_load(&g_dm_game_ver); if (!lv || ver < 0) return;
    if (lv != lv_seen) { lv_seen = lv; at = dm_guest_frame() + 30; return; } if (lv == lv_done || dm_guest_frame() < at) return; lv_done = lv;
    uint32_t lvno = *(uint32_t *)(lv + 0x1f8); if (lvno == 1 || lvno == 40 || lvno == 75 || lvno == 103 || lvno == 109) return;
    uint64_t map = *(uint64_t *)(lv + 0x1c8), cb = map ? *(uint64_t *)(map + 0x838) : 0; if (!cb) return;
    // 野外图上出口位不能直接用：8 个出口位按关卡的出口槽排，「走过去的相邻关卡」那几个槽的位几乎每间房都带（冰冷之原 0x70 = 三个相邻野外关卡），真有洞口的那间多带一个别的位（0x200）。
    // 逐间初始化再看出口链表在大图上太贵（火焰之河 306 间里 291 间带位，14.81 ms）。所以先数每个出口位落在几间房上：落在 8 间以上的是边界位，不看；只有带「少见位」的房间才初始化、看出口链表。
    // 地城图每个位只落在一间房上，筛法不变。整关房间都带同一组位的预设关卡筛不出候选——那类关卡原版本来就整关揭示。测试开关 DM_TEST_MAP_MARK_CHECK=1：再按老办法逐间全筛一遍，对不上记【红】。
    // On wilderness maps the exit bits cannot be used directly: the 8 exit bits are ordered by the level's exit slots, and the bits of the slots for "adjacent levels you walk into" are carried by almost every room (Cold Plains 0x70 = three adjacent wilderness levels), while the room that really has a cave mouth carries one more, different bit (0x200).
    // Initializing room by room and then looking at the exit linked list is too expensive on large maps (River of Flame: 291 of 306 rooms carry bits, 14.81 ms). So first count how many rooms each exit bit falls on: one that falls on 8 or more rooms is a border bit and is ignored; only rooms carrying a "rare bit" are initialized and have their exit linked list checked.
    // On dungeon maps each bit falls on only one room, so the filter does not change anything. Preset levels where every room carries the same set of bits yield no candidates -- the original game reveals those levels whole anyway. Test switch DM_TEST_MAP_MARK_CHECK=1: filter every room again the old way and log 【红】(red) on a mismatch.
    int total = 0, nwp = 0, nex = 0, ninit = 0, cnt[8] = {0}; uint32_t rare = 0; uint64_t t0 = w_now_ns();
    for (uint64_t r = *(uint64_t *)(lv + 0x10); r && total < 4096; r = *(uint64_t *)(r + 0x48), total++) { uint32_t fl = *(uint32_t *)(r + 0x50) >> 4 & 0xff; for (int i = 0; i < 8; i++) cnt[i] += fl >> i & 1; }
    for (int i = 0; i < 8; i++) if (cnt[i] && cnt[i] <= 8) rare |= 0x10u << i;
    total = 0;
    for (uint64_t r = *(uint64_t *)(lv + 0x10); r && total < 4096; r = *(uint64_t *)(r + 0x48), total++) { uint32_t fl = *(uint32_t *)(r + 0x50); int wp = (fl & 0x30000) != 0; if (!wp && !(fl & rare)) continue;
        ninit += !*(uint64_t *)(r + 0x58); ((void (*)(int, uint64_t))(b + 0x52dcc0))(ver, r); uint64_t ar = *(uint64_t *)(r + 0x58); if (!ar || (!wp && !*(uint64_t *)(r + 0x78))) continue;
        ((void (*)(uint64_t))(uintptr_t)cb)(ar); if (wp) nwp++; else nex++; }
    double ms = (w_now_ns() - t0) / 1e6;
    if (getenv("DM_TEST_MAP_MARK_CHECK")) { int miss = 0, all = 0, k = 0; for (uint64_t r = *(uint64_t *)(lv + 0x10); r && k < 4096; r = *(uint64_t *)(r + 0x48), k++) { uint32_t fl = *(uint32_t *)(r + 0x50); if (!(fl & 0xff0) || (fl & 0x30000)) continue;
            ((void (*)(int, uint64_t))(b + 0x52dcc0))(ver, r); if (*(uint64_t *)(r + 0x78)) { all++; if (!(fl & rare)) { miss++; LOG("【红】小地图核对：关卡 %u 房间 (%d,%d) 标志 %#x 有出口链表却没被筛中（少见位 %#x）", lvno, *(int32_t *)(r + 0x60) * 5, *(int32_t *)(r + 0x64) * 5, fl, rare); } } }
        LOG("小地图核对：关卡 %u 逐间全筛有出口链表的房间 %d 间，筛法漏 %d 间（少见位 %#x，各位间数 %d/%d/%d/%d/%d/%d/%d/%d）", lvno, all, miss, rare, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7]); }
    LOG("小地图：关卡 %u 进图标出 小站房间 %d 间、出入口房间 %d 间（共 %d 间房，提前初始化 %d 间，%.2f ms）", lvno, nwp, nex, total, ninit, ms);
}
// 小地图任务终点。
// 七个墓：法师峡谷里通往本局真墓（幕对象 +0x120）的墓口放任务日志的塔拉夏符号；其余终点放原版地图名牌（名字 + 灰十字）；画面外贴边只写名字（前端画，front_ios.m）。
// 终点位置全读游戏数据，不猜：
//   · 房间预设记录：客户端关卡房间（关卡 +0x10、下一间 +0x48）+0x98 = 链表第一条记录、下一条 +0x10（修：原先从第一条的 +0x10 起读，每间房漏掉第一条——召唤者 / 铁匠 / 封印时有时无）；记录 +0 = 0 物件 / 1 怪，+4 = 运行时类号，
//     +8 / +0x24 = 房内 x / y（召唤者实测：房间 (25000,5420) + (20,35) = 它的真实位置）。类号：物件 = objects 行号、怪 = monstats 行号、
//     超级独特 = 751 + 行号、地点怪 = 817 + 行号（表里的 Expansion 分隔行不占号）。
//   · 记录只在初始化过的房间里有：远处房间先调原版房间初始化 main+0x52dcc0（同出入口标注，不刷怪）。整关一次做完大图要 20 ms（熔岩之河 306 间），
//     所以按帧分摊：每帧做到 1.5 ms 为止，下一帧接着做（每间约 0.07–0.09 ms）。游戏过一阵会自己卸掉提前初始化的房间，找到的终点我们记成世界坐标、不依赖房间还在。
//   · 墓口 / 神庙入口：房间出口链表（+0x78）每项 +0 → 目标房间 → +0x90 关卡 → +0x1f8 关卡号。出口砖在房内的位置链表里没有：法师峡谷是固定预设图，
//     七个墓口砖在各自房间里的位置宿主逐个走到读出（探路工具 DM_TEST_WORLD_SEEK），按目标关卡写在 k_tomb_at。
// 尼拉塞克（沃特之厅 124）：记录里没有他本人，按他的出生点物件 462（objects.txt Class NihlathakOutsideTown「Nihlathak Start Outside Town」，四臂之一）定位——
// 四个种子把人逐段放到四条臂端实地看，尼拉塞克出生坐标与 462 的坐标逐格相同；整关唯一的地点怪 place_champion（820）总在他对面那条臂，不能用。
// 塔拉夏符号第 N 帧 = 关卡 66+N 墓门门楣上刻的符号（宿主逐门截图对过 66/67/68/70）。名牌号对应 tools/gen_map_goal_art.py 的 LABELS。
// Minimap quest goals.
// The seven tombs: in the Canyon of the Magi, the tomb entrance leading to this run's true tomb (act object +0x120) gets the Tal Rasha symbol from the quest log; other goals get the original game's map nameplate (name + gray cross); off-screen ones are pinned to the edge with the name only (drawn by the front end, front_ios.m).
// Goal positions are all read from game data, not guessed:
//   - Room preset records: client level room (level +0x10, next room +0x48) +0x98 = first record of the linked list, next record +0x10 (fix: it used to start reading from +0x10 of the first record, missing the first record of every room -- the Summoner / the Smith / the seals came and went); record +0 = 0 object / 1 monster, +4 = runtime class number,
//     +8 / +0x24 = x / y within the room (measured on the Summoner: room (25000,5420) + (20,35) = his real position). Class number: object = objects row number, monster = monstats row number,
//     super unique = 751 + row number, place monster = 817 + row number (the Expansion separator row in the tables does not take a number).
//   - Records exist only in initialized rooms: for distant rooms, first call the original game's room initialization main+0x52dcc0 (same as the entrance/exit marking, spawns no monsters). Doing a whole level at once costs 20 ms on a large map (River of Flame, 306 rooms),
//     so it is spread over frames: work up to 1.5 ms per frame and continue next frame (about 0.07–0.09 ms per room). After a while the game unloads the rooms initialized early by itself; goals we found are stored as world coordinates and do not depend on the room still being there.
//   - Tomb entrances / temple entrances: room exit linked list (+0x78), each entry +0 -> target room -> +0x90 level -> +0x1f8 level id. The position of the exit tile within the room is not in the linked list: the Canyon of the Magi is a fixed preset map,
//     the positions of the seven tomb entrance tiles within their rooms were read out on the host by walking to each one (path-probing tool DM_TEST_WORLD_SEEK) and are written in k_tomb_at by target level.
// Nihlathak (Halls of Vaught 124): the records do not contain him, so he is located by his spawn point object 462 (objects.txt Class NihlathakOutsideTown "Nihlathak Start Outside Town", one of the four arms) --
// with four seeds the player was placed step by step at the ends of the four arms to look on the spot, and Nihlathak's spawn coordinates equal those of 462 cell for cell; the level's only place monster place_champion (820) is always in the arm opposite him and cannot be used.
// Frame N of the Tal Rasha symbols = the symbol carved on the lintel of the tomb door of level 66+N (checked on the host by screenshots door by door for 66/67/68/70). Nameplate numbers correspond to LABELS in tools/gen_map_goal_art.py.
typedef struct { uint8_t lv, mon; uint16_t cls; uint8_t art; } mg_def;
static const mg_def k_mg[] = {
    { 17, 1, 822, 1 }, { 4, 0, 17, 2 }, { 4, 0, 18, 2 }, { 4, 0, 19, 2 }, { 4, 0, 20, 2 }, { 4, 0, 21, 2 }, { 4, 0, 22, 2 }, { 5, 0, 30, 3 },
    { 25, 1, 757, 4 }, { 28, 0, 108, 5 },                       // 女伯爵；赫拉迪克之锤 | the Countess; Horadric Malus
    { 49, 1, 761, 6 }, { 60, 0, 354, 7 }, { 64, 0, 356, 8 }, { 61, 0, 149, 9 }, { 74, 1, 250, 10 },   // 方块宝箱、权杖宝箱、污染太阳祭坛 | cube chest, staff chest, Tainted Sun Altar
    { 66, 0, 152, 23 }, { 67, 0, 152, 23 }, { 68, 0, 152, 23 }, { 69, 0, 152, 23 }, { 70, 0, 152, 23 }, { 71, 0, 152, 23 }, { 72, 0, 152, 23 },   // 法杖底座只在真墓里有（三个角色各在自己的真墓读到） | the staff holder exists only in the true tomb (three characters each read it in their own true tomb)
    { 78, 0, 252, 24 }, { 85, 0, 407, 11 }, { 91, 0, 406, 12 }, { 93, 0, 405, 13 }, { 83, 0, 404, 14 }, { 94, 0, 193, 15 },   // 吉德宾祭坛；卡林姆的晶体 / 智慧 / 生命核心宝箱 | Gidbinn altar; Khalim's Eye / Brain / Heart chests
    { 105, 1, 256, 17 }, { 107, 0, 376, 18 }, { 108, 0, 392, 19 }, { 108, 0, 393, 19 }, { 108, 0, 394, 19 }, { 108, 0, 395, 19 }, { 108, 0, 396, 19 },
    { 110, 1, 793, 20 }, { 111, 0, 473, 21 }, { 114, 0, 460, 22 },
    { 37, 1, 156, 25 }, { 73, 1, 211, 26 }, { 102, 1, 242, 27 }, { 131, 1, 543, 28 }, { 120, 0, 546, 29 }, { 124, 0, 462, 30 },   // 首领层与天坛（标任务怪物或宝箱的位置） | boss levels and the altar (marks the position of the quest monster or chest)
};
static const int8_t k_tomb_at[7][2] = { { 35, 35 }, { 29, 6 }, { 30, 6 }, { 24, 5 }, { 13, 32 }, { 14, 25 }, { 15, 32 } };   // 关卡 66–72 的墓口砖在入口房间内的位置 | position of the tomb entrance tile within the entrance room for levels 66–72
#define MG_SYM 64   // 终点图样号 ≥ 64：塔拉夏符号第（图样号 − 64）帧；其余 = 名牌号 | goal art number >= 64: frame (art number - 64) of the Tal Rasha symbols; otherwise = nameplate number
#define MG_MAX 12
static struct { uint64_t lv, room, ns; uint32_t lvno; int rooms, frames, n; int32_t x[MG_MAX], y[MG_MAX]; uint8_t art[MG_MAX]; } g_mg;
static _Atomic uint32_t g_mg_seq; static dm_map_goal_view g_mg_pub;
static int mg_wanted(uint32_t lvno) { if (lvno == 46 || lvno == 80) return 1; for (unsigned i = 0; i < sizeof k_mg / sizeof *k_mg; i++) if (k_mg[i].lv == lvno) return 1; return 0; }
static void mg_add(int x, int y, int art) {
    for (int i = 0; i < g_mg.n; i++) if (g_mg.art[i] == art && abs(g_mg.x[i] - x) < 25 && abs(g_mg.y[i] - y) < 25) return;   // 同一种终点挨着的只标一个（凯恩之石五块在一处，相距最远 15 格） | adjacent goals of the same kind get only one mark (the five Cairn Stones are in one place, at most 15 cells apart)
    if (g_mg.n < MG_MAX) { g_mg.x[g_mg.n] = x; g_mg.y[g_mg.n] = y; g_mg.art[g_mg.n] = (uint8_t)art; g_mg.n++; }
}
static void mg_scan_room(uint64_t lv, uint64_t r) {
    int rx = *(int *)(r + 0x60) * 5, ry = *(int *)(r + 0x64) * 5;
    uint64_t pl = *(uint64_t *)(r + 0x98);
    for (uint64_t u = pl, k = 0; u && k < 256; u = *(uint64_t *)(u + 0x10), k++)
        for (unsigned i = 0; i < sizeof k_mg / sizeof *k_mg; i++)
            if (k_mg[i].lv == g_mg.lvno && k_mg[i].mon == *(uint32_t *)u && k_mg[i].cls == *(uint32_t *)(u + 4)) mg_add(rx + *(int32_t *)(u + 8), ry + *(int32_t *)(u + 0x24), k_mg[i].art);
    if (g_mg.lvno != 46 && g_mg.lvno != 80) return;
    uint64_t am = g_mg.lvno == 80 ? 0 : *(uint64_t *)(lv + 0x1c8); if (g_mg.lvno != 80 && !am) return;   // 幕级对象读不到：这次不扫（原先不判空，为空就去读地址 0x120；同文件别处读它都判了） | Act-level object cannot be read: skip this scan (previously there was no null check and a null led to reading address 0x120; every other read of it in this file checks)
    uint32_t want = g_mg.lvno == 80 ? 94 : *(uint32_t *)(am + 0x120);
    for (uint64_t e = *(uint64_t *)(r + 0x78), k = 0; e && k < 16; e = *(uint64_t *)(e + 8), k++) {
        uint64_t tr = *(uint64_t *)e, tl = tr ? *(uint64_t *)(tr + 0x90) : 0; uint32_t to = tl ? *(uint32_t *)(tl + 0x1f8) : 0;
        if (!want || to != want) continue;
        if (to >= 66 && to <= 72) mg_add(rx + k_tomb_at[to - 66][0], ry + k_tomb_at[to - 66][1], MG_SYM + (int)(to - 66));
        else mg_add(rx + *(int *)(r + 0x68) * 5 / 2, ry + *(int *)(r + 0x6c) * 5 / 2, 16);   // 市集 → 毁灭的神庙：入口在房间中部 | Kurast Bazaar -> Ruined Temple: the entrance is in the middle of the room
    }
}
static void w_map_goal_step(void) {
    static int env = -2; if (env == -2) { const char *e = getenv("DM_MAP_MARK"); env = e ? e[0] != '0' : -1; }
    int dm_custom_get(const char *key); int on = env >= 0 ? env : dm_custom_get("map_mark");
    uint8_t *b = wb(); uint64_t pl = on && b ? w_player() : 0, lv = pl ? w_level(pl) : 0; int ver = atomic_load(&g_dm_game_ver);
    if (lv != g_mg.lv) { memset(&g_mg, 0, sizeof g_mg); g_mg.lv = lv; if (lv) { g_mg.lvno = *(uint32_t *)(lv + 0x1f8); g_mg.room = mg_wanted(g_mg.lvno) ? *(uint64_t *)(lv + 0x10) : 0; } }
    if (g_mg.room && ver >= 0) {
        uint64_t t0 = w_now_ns(); g_mg.frames++;
        while (g_mg.room && w_now_ns() - t0 < 1500000) { uint64_t r = g_mg.room; if (!*(uint64_t *)(r + 0x58)) ((void (*)(int, uint64_t))(b + 0x52dcc0))(ver, r); mg_scan_room(lv, r); g_mg.rooms++; g_mg.room = *(uint64_t *)(r + 0x48); }
        g_mg.ns += w_now_ns() - t0;
        if (!g_mg.room) { char o[160] = ""; int k = 0; for (int i = 0; i < g_mg.n; i++) k += snprintf(o + k, sizeof o - k, " %d@%d,%d", g_mg.art[i], g_mg.x[i], g_mg.y[i]);
            LOG("小地图终点：关卡 %u 扫了 %d 间房，分 %d 帧共 %.2f ms，终点 %d 个（图样号@格）%s", g_mg.lvno, g_mg.rooms, g_mg.frames, g_mg.ns / 1e6, g_mg.n, o); } }
    // 发布：玩家世界格、玩家在画面上的位置（脚底，归一化）、终点；前端按地图几何（dm_ui_map_geom）换算到画面
    // Publish: player world cell, player position on screen (feet, normalized), goals; the front end converts to the screen by the map geometry (dm_ui_map_geom)
    dm_map_goal_view v; memset(&v, 0, sizeof v);
    if (lv && !g_mg.room && g_mg.n) { int px = 0, py = 0; w_unit_xy(pl, &px, &py); v.px = px; v.py = py; int W = 0, H = 0; dm_screen_real(&W, &H); uint64_t scene = *(uint64_t *)(b + 0x2e2a698); float f[2];
        if (W > 0 && H > 0 && scene && w_project(b, scene, pl, f, 0)) { v.sx = f[0] / W; v.sy = f[1] / H; v.n = g_mg.n; for (int i = 0; i < g_mg.n; i++) { v.x[i] = g_mg.x[i]; v.y[i] = g_mg.y[i]; v.art[i] = g_mg.art[i]; } } }
    atomic_fetch_add(&g_mg_seq, 1); g_mg_pub = v; atomic_fetch_add(&g_mg_seq, 1);
}
int dm_map_goals(dm_map_goal_view *v) {
    for (int t = 0; t < 4; t++) { uint32_t a = atomic_load(&g_mg_seq); if (a & 1) continue; *v = g_mg_pub; if (atomic_load(&g_mg_seq) == a) return v->n; }
    v->n = 0; return 0;
}
void dm_world_pump(void) {   // 取手柄处每帧调 | Called every frame at the controller poll
#if TARGET_OS_OSX
    w_mouse_cancel_pending();
#endif
    { void dm_rift_client_pump(uint8_t *); DM_PROF(0, "w.rift_client", dm_rift_client_pump(wb())); }   // 秘境爬塔：认击杀（dm_rift.c） | rift tower climb: recognize kills (dm_rift.c)
    { int nd = atomic_load(&g_dm_dur_sync_n); uint8_t *b0 = nd ? wb() : NULL; if (b0) { atomic_store(&g_dm_dur_sync_n, 0); for (int i = 0; i < nd && i < 16; i++) { uint64_t c = w_find(4, g_dm_dur_sync[i][0]); if (c) ((void (*)(uint64_t, int, int, int))(b0 + 0x5bc1f0))(c, 72, (int)g_dm_dur_sync[i][1], 0); } } }   // DM_TEST_DUR 的客户端一半（diag.c） | client half of DM_TEST_DUR (diag.c)
    DM_PROF(0, "w.test_seek", w_test_seek_step());
    w_test_xy_step();
    uint8_t *b = wb(); if (!b) return;
    DM_PROF(0, "w.cam", w_cam_clear());
    if (g_it.active) w_interaction_poll(w_player(), dm_guest_frame());
    DM_PROF(0, "w.watch", w_watch_step(b));
    { void dm_potpick_step(uint8_t *); DM_PROF(0, "w.potpick", dm_potpick_step(b)); }   // 药水自动拾取（dm_potpick.c） | automatic potion pickup (dm_potpick.c)
    { void dm_padmode_step(uint8_t *, uint64_t); DM_PROF(0, "w.padmode", dm_padmode_step(b, dm_guest_frame())); }   // 手柄 / 触屏布局切换（dm_padmode.c） | Controller / touch layout switching (dm_padmode.c)
    { void dm_beltauto_step(uint8_t *); DM_PROF(0, "w.beltauto", dm_beltauto_step(b)); }   // 开背包自动补腰带（dm_beltauto.c） | Auto-refill the belt when the inventory opens (dm_beltauto.c)
    { void dm_pouch_step(uint8_t *); DM_PROF(0, "w.pouch", dm_pouch_step(b)); }   // 百宝囊 / 存放（dm_pouch.c） | pouch / deposit (dm_pouch.c)
    if (g_release_at && dm_guest_frame() >= g_release_at) { g_release_at = 0; uint64_t pl = w_player(); if (pl) { atomic_store(&g_dm_move_allow_until_ns, w_now_ns() + 500000000ull); ((int (*)(uint64_t, int, int, int, uint64_t))(b + 0x4ace20))(pl, 2, g_rel_x, g_rel_y, 0); } }
    DM_PROF(0, "w.go", w_go_step());
    DM_PROF(0, "w.tile_retry", w_tile_retry_step());
    DM_PROF(0, "w.map_mark", w_map_mark_step());
    DM_PROF(0, "w.map_goal", w_map_goal_step());
    // 测试开关 DM_TEST_WORLD_TAP（及 TAP2 / TAP3 / TAP4）=<类型>:c<类号>@<拍>（或 <类型>:any@<拍> = 这一类里任意一个，物品只认地上的）：到这一拍起盯住这类单位，它一进画面就在它身体框中心落一次世界点按（走 dm_world_tap 的真实命中路径，
    // 不是直接指定目标）——Mac 宿主上验「点得中、走得到、开得了」用（iOS 包用触点脚本的 tap_unit，同一个量具）。5 秒内没进画面报一行并放弃。
    // Test switch DM_TEST_WORLD_TAP (and TAP2 / TAP3 / TAP4)=<type>:c<class>@<tick> (or <type>:any@<tick> = any one of this type, items only count when on the ground): from this tick on, watch this kind of unit, and as soon as it enters the picture drop one world tap at the center of its body box (going through the real hit path of dm_world_tap,
    // not naming the target directly) -- used on the Mac host to verify "can be hit, can be walked to, can be opened" (the iOS app uses tap_unit in the touch point script, the same probe). If it has not entered the picture within 5 seconds, report one line and give up.
    for (int tk = 0; tk < 4; tk++) { static int st[4]; static uint32_t t0[4]; static const char *tn[4] = {"DM_TEST_WORLD_TAP", "DM_TEST_WORLD_TAP2", "DM_TEST_WORLD_TAP3", "DM_TEST_WORLD_TAP4"};
        const char *e = st[tk] == 2 ? NULL : getenv(tn[tk]); int ty = 0, at = 0; unsigned cl = 0; uint32_t dm_cur_tick(void);
        if (!e || (sscanf(e, "%d:c%u@%d", &ty, &cl, &at) != 3 && !(sscanf(e, "%d:any@%d", &ty, &at) == 2 && (cl = 0xffffff)))) { st[tk] = 2; continue; }
        uint32_t now = dm_cur_tick(); if (now < (uint32_t)at) continue;
        if (!st[tk]) { if (atomic_load(&g_watch)) continue; dm_world_watch(ty, cl); st[tk] = 1; t0[tk] = now; continue; }   // 一次只盯一个：前一个没完就等 | watch only one at a time: wait if the previous one is not finished
        float nx, ny; if (dm_world_watch_pos(&nx, &ny)) { dm_world_watch(-1, 0); st[tk] = 2; LOG("世界目标：%s %s → 在画面 (%.3f,%.3f) 落点按", tn[tk], e, nx, ny); dm_world_tap(nx, ny);
            const char *sp = getenv("DM_TEST_WORLD_TAP_SHOT"); if (sp) { void dm_gfx_shot_at(long, const char *); char pth[512]; snprintf(pth, sizeof pth, "%s%d.png", sp, tk + 1); dm_gfx_shot_at(1, pth); } }   // 同时截一张（路径前缀 + 序号），和候选框日志对照 | also take a screenshot (path prefix + sequence number) to compare against the candidate-box log
        else if (now - t0[tk] > 125) { dm_world_watch(-1, 0); st[tk] = 2; LOG("世界目标：%s %s → 5 秒内这类单位没进画面，放弃", tn[tk], e); } }
    // 测试开关 DM_TEST_WORLD_GO（及同语法的 DM_TEST_WORLD_GO2 / 3 / 4，给多步场景）=<类型>:<id>[@拍] 或 <类型>:c<类号>[@拍]：进局第 200 拍（或指定拍）对该单位发一次「走过去交互」（验商人页等要先走到 NPC 的场景）。
    // 前缀 near:（如 near:1:c155；near2: = 2 格）= 只走到目标 4 格（或指定格数）内就停、不交互。另两种写法：monster[@拍] = 走到最近的敌对怪旁边（5 格内停下，不交互、不攻击）；exit[@拍] = 走到最近的出口并交互（进洞 / 换层）。只在客户端已载入的附近房间里找。
    // 给脚本请用类号写法（如 1:c147 = 基德）：单位 id 是每局按生成顺序发的，换存档 / 换角色 / 换随机种子就变（同一营地里基德在 Mac 宿主标准局是 id 6，在 iOS 包那局是 id 5），类号不变；同类有多个时取离玩家最近的
    // Test switch DM_TEST_WORLD_GO (and DM_TEST_WORLD_GO2 / 3 / 4 with the same syntax, for multi-step scenes) =<type>:<id>[@tick] or <type>:c<class id>[@tick]: at tick 200 after joining (or the given tick), issue one "walk over and interact" on that unit (for verifying scenes such as the vendor page where you must first walk to the NPC).
    // Prefix near: (e.g. near:1:c155; near2: = 2 cells) = only walk to within 4 cells of the target (or the given number of cells) and stop, without interacting. Two other forms: monster[@tick] = walk up next to the nearest hostile monster (stop within 5 cells, no interaction, no attack); exit[@tick] = walk to the nearest exit and interact (enter the cave / change floor). Searches only in nearby rooms the client has loaded.
    // For scripts please use the class id form (e.g. 1:c147 = Gheed): unit ids are handed out per run in spawn order and change with a different save / character / random seed (in the same camp Gheed is id 6 in the Mac host standard run and id 5 in that iOS app run), while class ids do not; when there are several of the same class, the one nearest the player is taken
    for (int gk = 0; gk < 4; gk++) { static int tgs[4]; static const char *gn[4] = {"DM_TEST_WORLD_GO", "DM_TEST_WORLD_GO2", "DM_TEST_WORLD_GO3", "DM_TEST_WORLD_GO4"}; const char *e = tgs[gk] ? NULL : getenv(gn[gk]); uint32_t dm_cur_tick(void); int gt = 0, gi = 0, at = 200, bycls = 0;
      int nearonly = 0; if (e && !strncmp(e, "near", 4) && (e[4] == ':' || (e[4] >= '1' && e[4] <= '9' && e[5] == ':'))) { nearonly = e[4] == ':' ? W_NEAR : e[4] - '0'; e += e[4] == ':' ? 5 : 6; }   // 前缀 near: = 只走到旁边（4 格内）不交互（验「到了旁边按主钮」用） | Prefix near: = only walk up next to it (within 4 cells) without interacting (used to verify "press the main button once next to it")
      int kind = 0; if (e && !strncmp(e, "monster", 7)) { kind = 1; gt = 1; sscanf(e + 7, "@%d", &at); } else if (e && !strncmp(e, "exit", 4)) { kind = 2; gt = 5; sscanf(e + 4, "@%d", &at); }
      if (e && (kind || (sscanf(e, "%d:c%d@%d", &gt, &gi, &at) >= 2 ? (bycls = 1) : sscanf(e, "%d:%d@%d", &gt, &gi, &at) >= 2)) && dm_cur_tick() >= (uint32_t)at && w_player() && gt >= 0 && gt <= 5) { tgs[gk] = 1; uint64_t u = 0;
          if (kind) { uint64_t pl2 = w_player(); int px = 0, py = 0, x, y; long bd = -1; w_unit_xy(pl2, &px, &py);   // monster = 最近的敌对怪（可选中，游戏的交互类别是怪而不是 NPC）；exit = 最近的出口砖（类型 5） | monster = nearest hostile monster (selectable, and the game's interaction category is monster rather than NPC); exit = nearest exit tile (type 5)
              for (int k = 0; k < 128; k++) for (uint64_t c = *(uint64_t *)(b + 0x2d603a0 + gt * 0x400 + k * 8); c; c = *(uint64_t *)(c + 0x158)) {
                  if (!(((int (*)(uint64_t, int, int, int, int))(b + 0x464a70))(c, 0, 0, -1, -1) & 1) || !w_unit_xy(c, &x, &y)) continue;
                  if (kind == 1) { int ct = ((int (*)(int, uint64_t, uint64_t))(b + 0x2f7b7c))(0x165, pl2, c); if (ct != 1 && ct != 0xa) continue; }   // 类别 1 = 普通怪（运行时读出：野外的怪都是 1；营地 NPC 是 5），0xa = 另一类可攻击目标 | Category 1 = ordinary monster (read at runtime: monsters in the wilderness are all 1; camp NPCs are 5), 0xa = another kind of attackable target
                  long dd = (long)(x - px) * (x - px) + (long)(y - py) * (y - py); if (bd < 0 || dd < bd) { bd = dd; u = c; } }
              if (!u) { int shown = 0; for (int k = 0; k < 128 && shown < 8; k++) for (uint64_t c = *(uint64_t *)(b + 0x2d603a0 + gt * 0x400 + k * 8); c && shown < 8; c = *(uint64_t *)(c + 0x158)) { int cx2 = 0, cy2 = 0; w_unit_xy(c, &cx2, &cy2); shown++;
                  LOG("世界目标：  在场 类型 %d 类号 %u 模式 %u 可选 %d 类别 %d 距 (%d,%d)", gt, *(uint32_t *)(c + 4), *(uint32_t *)(c + 0xc), ((int (*)(uint64_t, int, int, int, int))(b + 0x464a70))(c, 0, 0, -1, -1) & 1, ((int (*)(int, uint64_t, uint64_t))(b + 0x2f7b7c))(0x165, pl2, c), cx2 - px, cy2 - py); } }
              if (u) { w_go(u); if (kind == 1) g_go.stop_at = 5; else if (nearonly) g_go.stop_at = nearonly; } LOG("世界目标：%s %s → %s", gn[gk], e, u ? "已发" : "没找到（附近已载入的房间里没有）"); if (u) LOG("世界目标：%s 目标 类型 %u 类号 %u id %u", gn[gk], *(uint32_t *)u, *(uint32_t *)(u + 4), *(uint32_t *)(u + 8)); continue; }
          if (!bycls) u = w_find((uint32_t)gt, (uint32_t)gi);
          else { int px = 0, py = 0, x, y; long bd = -1; w_unit_xy(w_player(), &px, &py);
              for (int k = 0; k < 128; k++) for (uint64_t c = *(uint64_t *)(b + 0x2d603a0 + gt * 0x400 + k * 8); c; c = *(uint64_t *)(c + 0x158)) if (*(uint32_t *)(c + 4) == (uint32_t)gi && w_unit_xy(c, &x, &y)) { long dd = (long)(x - px) * (x - px) + (long)(y - py) * (y - py); if (bd < 0 || dd < bd) { bd = dd; u = c; } } }
          if (u) { w_go(u); if (nearonly) g_go.stop_at = nearonly; } LOG("世界目标：DM_TEST_WORLD_GO %s → %s", e, u ? "已发" : "没找到"); if (u) LOG("世界目标：DM_TEST_WORLD_GO 目标 类型 %u 类号 %u id %u", *(uint32_t *)u, *(uint32_t *)(u + 4), *(uint32_t *)(u + 8)); } }
    w_tap_step(b);
#if TARGET_OS_OSX
    { uint64_t dm_stick_last_ns(void); uint64_t v = dm_stick_last_ns(); if (v != g_gnd_stick_seen) { g_gnd_stick_seen = v; g_gnd_stick_at = w_now_ns(); } }
    w_walk_step(b);
#endif
    { uint64_t pl = w_player(); atomic_store(&g_has_target, pl && ((uint64_t (*)(uint64_t, uint32_t))(b + 0x2f7a0c))(((uint64_t (*)(void))(b + 0x2acecc))(), *(uint32_t *)(b + 0x2d20e4c)) != 0); }
}
