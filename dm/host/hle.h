// HLE 公共：按 Switch 改编名登记宿主实现。
// 注意苹果 AAPCS64 与 Switch 的差异：苹果要求调用方把 <32 位参数（bool/char/short）扩展好，Switch 代码不扩展 ⇒
// 这类参数一律声明成 int/unsigned 再自己截断。可变参数苹果走栈、Switch 走寄存器 ⇒ 可变参数函数不能直接用 C 的 "..."。
// HLE common: register host implementations by Switch mangled name.
// Note the differences between Apple AAPCS64 and Switch: Apple requires the caller to extend <32-bit arguments (bool/char/short), Switch code does not extend ⇒
// such parameters are always declared as int/unsigned and truncated by ourselves. Variadic arguments go on the stack on Apple and in registers on Switch ⇒ variadic functions cannot use C's "..." directly.
#pragma once
#include "dm.h"
#include "dm_sect.h"
typedef struct { const char *name; void *fn; } hle_ent;
#define HLE(mangled, ret, cname, ...)                                                             \
    static ret cname(__VA_ARGS__);                                                                \
    DM_SECT(swhle) static const hle_ent hle_ent_##cname = {mangled, (void *)cname}; \
    static ret cname(__VA_ARGS__)

typedef uint32_t Result;   // nn::Result：w0，0 = 成功 | nn::Result: w0, 0 = success
#define RES_OK 0u
#define RES_FAIL 0x2A2u      // 通用失败（联网等被剪功能统一回这个） | Generic failure (cut features such as networking uniformly return this)

// 客体地址 → 宿主对象 旁表
// Guest address → host object side table
void *objmap_get(const void *guest);
void objmap_put(const void *guest, void *host);
void *objmap_del(const void *guest);
uint64_t now_ns(void);
uint64_t guest_ns(void);              // 客体时钟（hle_os.c；DM_TICK_HOLD 冻结时不走） | Guest clock (hle_os.c; does not advance while DM_TICK_HOLD is frozen)
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
uint64_t dm_guest_frame(void);        // 宿主每次出图递增；不受机器负载影响 | Incremented each time the host presents a frame; unaffected by machine load
uint64_t dm_server_tick(void);        // 局内服务端拍；不在局内或 DM_TICK_HOLD 冻结中返回 UINT64_MAX | In-run server tick; returns UINT64_MAX when not in a run or while DM_TICK_HOLD is frozen
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // 按原因冻结（1 后台，2 系统键盘）；见 hle_os.c | Freeze by reason (1 background, 2 system keyboard); see hle_os.c

// 强制接管：即使 SDK 模块里有定义，装载时所有对该符号的引用（JUMP_SLOT/GLOB_DAT/ABS64）一律解析到宿主实现。
// 只用于服务层 nn::（fs/oe/aoc/pctl… 的服务客户端）；SDK 的 libc++、nn::os 内部不进这张表。
// 每条必须写接管理由；表里有、但没有任何模块引用的条目，装载时报红中止（清单写错要响亮）。
// Forced takeover: even if the SDK module has a definition, all references to that symbol at load time (JUMP_SLOT/GLOB_DAT/ABS64) are resolved to the host implementation.
// Used only for the service-layer nn:: (service clients of fs/oe/aoc/pctl...); the SDK's libc++ and nn::os internals do not go into this table.
// Every entry must state the reason for the takeover; an entry in the table that no module references makes loading report a failure and abort (a wrong list must fail loudly).
typedef struct { const char *name; void *fn; const char *why; } hle_force_ent;
#define HLE_FORCE(mangled, why, ret, cname, ...)                                                   \
    static ret cname(__VA_ARGS__);                                                                \
    DM_SECT(swforce) static const hle_force_ent hle_force_##cname = {mangled, (void *)cname, why}; \
    static ret cname(__VA_ARGS__)

// 运行模式（0 掌机 / 1 主机）：hle_oe.c 持有，hid 的手柄样式跟它走；前端可在运行中切换
// Operation mode (0 handheld / 1 docked): owned by hle_oe.c, and the hid controller style follows it; the front end can switch it at runtime
int dm_opmode(void);
void dm_3d_scale_hot(int hot);      // 过热时把 3D 渲染比例降到 0.70（界面不动），0 = 恢复 | When overheating, lower the 3D render scale to 0.70 (UI untouched); 0 = restore
ssize_t dm_data_pread(int fd, void *buf, size_t n, off_t off);   // 资源档案的读：有「.holes」区段表的剔零档案按表换算（hle_fs.c） | Resource archive reads: zero-stripped archives with a ".holes" extent table are translated through the table (hle_fs.c)
void dm_data_note_fd(int fd, const char *hostpath);
int dm_overlay_file(const char *rel, char *out, size_t n);   // 覆盖层里的文件：DM_ROMFS_OVERLAY 可以有多层（冒号分隔，前面的优先），逐层找 <层>/<rel>，找到写进 out 返回 1（hle_fs.c） | File in the overlay: DM_ROMFS_OVERLAY may have several layers (colon-separated, earlier ones take priority); look for <layer>/<rel> layer by layer, and on a hit write it to out and return 1 (hle_fs.c)
void dm_set_opmode(int m);          // 变化时按游戏打开的开关推送通知 30 / 31 | On change, push notification 30 / 31 according to the switches the game has turned on
// 焦点（1 前台 / 2 失焦 / 3 后台）：前端在生命周期变化时调，变化即推送通知 15
// Focus (1 foreground / 2 focus lost / 3 background): called by the front end on lifecycle changes; a change pushes notification 15
void dm_focus_set(int st);
// 音频随生命周期暂停 / 恢复（前端在切后台 / 回前台时调）
// Audio pause / resume following the lifecycle (called by the front end on going to background / returning to foreground)
void dm_audio_suspend(int on);
uint64_t dm_audio_render_calls(void);   // 声卡回调累计次数（静音接收端不计） | Cumulative sound card callback count (the silent sink is not counted)

// ---------- 宿主调用号登记表（剪口把游戏内部函数入口换成 `svc #号; ret`，改写器把它变成宿主门）----------
// Horizon 的 SVC 号只用到 0x7F，0x80 起留作宿主调用。新增必须在这里登记，号码不得重复；分派在 hle_svc.c → svc_host_call。
//   号     用途                                         所在文件        剪口（main 偏移）
//   0x80   TACT BLTE 块 zlib 解码 → libdeflate           hle_tact.c      0x1b0ee60（DecodeZlib 入口）
//   0x81   客户端命令包投递（收包泵 → 服务端收包）         diag.c          0x5d3a88（bl 0x632510 调用点；缺省原样转调，DM_C2S_REPLAY 时丢弃）
//   0x82   系统类客户端消息投递（收包泵 → 0x6310b0）       diag.c          0x5d39c8（bl 0x6310b0 调用点；缺省原样转调，DM_C2S_REPLAY 时文件首拍起丢弃）
//   0x83   Granny 段逐块解码（段读取 0x11f0110）           hle_granny.c    0x11f02dc（bl 0x11f4af0 调用点；缺省按预解码候选替换，核不上即原样转调）
//   0x84   Granny 修正表逐块解码（段读取 0x11f07d0）       hle_granny.c    0x11f0914（bl 0x11f4af0 调用点；同上）
//   0x85   私有技能 HUD 自绘查询（只读，写 x8）          hle_tact.c      0x3e443c（分支门：x8≠0 跳 0x3e4460，否则执行原指令回 0x3e4440；仅触屏私有门）
//   0x86   私有技能地面方向（转调原 0x2b4ef8 再按右摇杆改向） hle_private_aim.c 0x2faf04（bl 0x2b4ef8 调用点；仅触屏私有门）
//   0x87   地上物品外观路径（转调原 0xa14dc4 再按规则改指带光变体） dm_dropfilter.c 0x8fa590（bl 0xa14dc4 调用点；只改模式 3 / 5 的物品，输出在 x8 字符串）
//   0x88   物品落地掉落音（转调原 0x470c20 再把一档物品换成提示音） dm_dropfilter.c 0x468f2c（bl 0x470c20 调用点；写 x0 = 声音号）
//   0x89   秘境出口改道（秘境局里把目的地换成下一张图，写 x2 / x3 / x8） dm_rift.c      0x7cc1e0（分支门：x8≠0 跳 0x634000，否则执行原指令回 0x7cc1e4）
//   0x8a   进局给玩家取出生点（转调原 0x5596c0；进局在城镇时换成小站旁）  dm_townwp.c     0x5d8040（bl 0x5596c0 调用点；玩家进局函数 0x5d7f70 里）
//   0x8b   进局给尸体取落点（同上，与玩家取同一个点）                      dm_townwp.c     0x5d8400（bl 0x5596c0 调用点；同一函数里）
//   0x8c   秘境里存档的当前幕（人在秘境图里把 w9 换成 4 = 第五幕，写 x9；x8 恒 0） dm_rift.c      0x6fa9d0（分支门：x8≠0 跳 0x6fa9d4，否则执行原指令回 0x6fa9d4）
//   0x8d   佣兵加装备位（佣兵 + 秘境 ≥10 层 + 腰带 / 鞋 / 手套 / 戒指 / 项链时把 w0 换成 1，写 x0；x8 恒 0） dm_rift.c      0x64a34c（分支门：x8≠0 跳 0x64a350，否则执行原指令回 0x64a350）
//   0x8e   佣兵加装备位的客户端校验（秘境 ≥10 层 + 物品是腰带 / 鞋 / 手套 / 戒指 / 项链时把 w1 换成物品自己的类型号，写 x1；x8 恒 0） dm_rift.c      0x3beb58（分支门：x8≠0 跳 0x3beb5c，否则执行原指令回 0x3beb5c）
//   0x8f   原版画面往 3D 镜头前方挪（转调原 0x1628978，再把镜头位置与偏移加上挪动量） dm_cam.c      0x89ccc8（bl 0x1628978 调用点；镜头每帧更新 0x89c370 里）
// ---------- Host call number registry (a cut replaces a game-internal function entry with `svc #number; ret`, and the rewriter turns it into a host gate) ----------
// Horizon's SVC numbers only go up to 0x7F; 0x80 onward is reserved for host calls. New ones must be registered here and numbers must not repeat; dispatch is in hle_svc.c → svc_host_call.
//   No.    Purpose                                      File            Cut (main offset)
//   0x80   TACT BLTE block zlib decode → libdeflate           hle_tact.c      0x1b0ee60 (DecodeZlib entry)
//   0x81   Client command packet delivery (receive pump → server receive)         diag.c          0x5d3a88 (bl 0x632510 call site; forwarded as is by default, dropped under DM_C2S_REPLAY)
//   0x82   System-class client message delivery (receive pump → 0x6310b0)       diag.c          0x5d39c8 (bl 0x6310b0 call site; forwarded as is by default, dropped from the file's first tick on under DM_C2S_REPLAY)
//   0x83   Granny section per-block decode (section read 0x11f0110)           hle_granny.c    0x11f02dc (bl 0x11f4af0 call site; by default replaced with the pre-decoded candidate, forwarded as is if it does not check out)
//   0x84   Granny fixup table per-block decode (section read 0x11f07d0)       hle_granny.c    0x11f0914 (bl 0x11f4af0 call site; same as above)
//   0x85   Private skill HUD self-draw query (read-only, writes x8)          hle_tact.c      0x3e443c (branch gate: x8≠0 jumps to 0x3e4460, otherwise executes the original instruction and returns to 0x3e4440; touch-only private gate)
//   0x86   Private skill ground direction (forwards to the original 0x2b4ef8, then redirects by the right stick) hle_private_aim.c 0x2faf04 (bl 0x2b4ef8 call site; touch-only private gate)
//   0x87   Ground item appearance path (forwards to the original 0xa14dc4, then repoints by rule to the lit variant) dm_dropfilter.c 0x8fa590 (bl 0xa14dc4 call site; only items in mode 3 / 5 are changed, output in the x8 string)
//   0x88   Item landing drop sound (forwards to the original 0x470c20, then swaps tier-one items to a notification sound) dm_dropfilter.c 0x468f2c (bl 0x470c20 call site; writes x0 = sound id)
//   0x89   Rift exit rerouting (in a rift run the destination is replaced with the next map, writes x2 / x3 / x8) dm_rift.c      0x7cc1e0 (branch gate: x8≠0 jumps to 0x634000, otherwise executes the original instruction and returns to 0x7cc1e4)
//   0x8a   Spawn point for the player on run entry (forwards to the original 0x5596c0; replaced with a point next to the waypoint when entering in town)  dm_townwp.c     0x5d8040 (bl 0x5596c0 call site; in the player join function 0x5d7f70)
//   0x8b   Drop point for the corpse on run entry (same as above, the same point as the player gets)                      dm_townwp.c     0x5d8400 (bl 0x5596c0 call site; in the same function)
//   0x8c   Current act saved while in a rift (when the character is in a rift map w9 is replaced with 4 = Act V, writes x9; x8 always 0) dm_rift.c      0x6fa9d0 (branch gate: x8≠0 jumps to 0x6fa9d4, otherwise executes the original instruction and returns to 0x6fa9d4)
//   0x8d   Extra mercenary equipment slots (mercenary + rift level ≥10 + belt / boots / gloves / ring / amulet: w0 is replaced with 1, writes x0; x8 always 0) dm_rift.c      0x64a34c (branch gate: x8≠0 jumps to 0x64a350, otherwise executes the original instruction and returns to 0x64a350)
//   0x8e   Client-side check for the extra mercenary equipment slots (rift level ≥10 + the item is a belt / boots / gloves / ring / amulet: w1 is replaced with the item's own type number, writes x1; x8 always 0) dm_rift.c      0x3beb58 (branch gate: x8≠0 jumps to 0x3beb5c, otherwise executes the original instruction and returns to 0x3beb5c)
//   0x8f   Original picture moved ahead of the 3D camera (forwards to the original 0x1628978, then adds the shift to the camera position and offset) dm_cam.c      0x89ccc8 (bl 0x1628978 call site; in the per-frame camera update 0x89c370)
enum { SVC_HOST_TACT_ZLIB = 0x80, SVC_HOST_C2S_DELIVER = 0x81, SVC_HOST_C2S_SYS = 0x82, SVC_HOST_GRANNY_A = 0x83, SVC_HOST_GRANNY_B = 0x84, SVC_HOST_PRIVATE_SKILL_RENDER = 0x85, SVC_HOST_PRIVATE_SKILL_AIM = 0x86, SVC_HOST_GROUND_ASSET = 0x87, SVC_HOST_DROP_SOUND = 0x88, SVC_HOST_RIFT_EXIT = 0x89, SVC_HOST_JOIN_SPAWN = 0x8a, SVC_HOST_JOIN_CORPSE = 0x8b, SVC_HOST_SAVE_ACT = 0x8c, SVC_HOST_MERC_SLOT = 0x8d, SVC_HOST_MERC_SLOT_CL = 0x8e, SVC_HOST_CAM_AHEAD = 0x8f };   // 0x87：地上物品取模型路径（dm_dropfilter.c 掉落光）   // 0x83/0x84：Granny 逐块解码调用点（hle_granny.c） | 0x87: fetch the model path for ground items (dm_dropfilter.c drop light)   // 0x83/0x84: Granny per-block decode call sites (hle_granny.c)
