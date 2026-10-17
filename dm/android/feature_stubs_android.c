/* P2 中和桩：dm_cursor.c 引用的"尚未移植"功能子系统（自定义页 / 极乐世界 / 免回城 / 秘境 / 编年史 /
 * 符文之语 / 百宝囊 / 自动喝药捡药 / 手柄↔触屏切换（padmode）/ 诊断计数 …）。
 * 语义 = 全部取"无此功能"的中性值：行为等价于这些剪口引入之前，P8 各段再把真实现逐个换上来。
 * 每个桩都注明真实实现所在文件与后续阶段。签名逐一对照头文件/调用点核对。
 * Neutral stubs for the feature subsystems dm_cursor.c references but Android has not ported yet.
 * All return the "feature absent" value; the real implementations land in later phases (P8). */
#include "../host/hle.h"
#include "../host/dm.h"
#include "../host/dm_cursor.h"
#include "../host/dm_cursor_item.h"
#include <stdatomic.h>

/* ---- dm_custom.c（P8d：选项→自定义页）---- */

/* ---- dm_elysium.c（P8c：极乐世界模式）---- */

/* ---- dm_freetp.c（P8b：免费回城）---- */
/* ---- diag.c 的冻结/服务端物品读数（P2 只用到中性值）---- */
_Atomic int g_dm_frz_want;
void dm_freeze_frame(int want) { (void)want; }
uint32_t dm_cur_tick(void) { return (uint32_t)dm_guest_frame(); }   /* 无 diag 的拍读，用出图帧数当单调拍号 | no diag tick read; use the guest frame counter as a monotonic tick */
uint64_t dm_diag_srv_item(uint32_t id) { (void)id; return 0; }
/* ---- dm_prof.c 的 IPF 计数器（关）---- */

/* ---- gfx 截图探针（关）---- */
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }
/* ---- dm_kills.c / dm_rift_ui.c / dm_runeword.c / dm_pouch.c / dm_potpick.c / dm_beltauto.c（P8）---- */

/* ---- dm_vault_ui.c（P8a：编年史仓库）---- */
/* ---- dm_padmode.c（P4：手柄↔触屏；此处恒"触屏且不忙"）---- */
/* ---- dm_pages.c（P8a：个人仓库多页）---- */
/* ---- dm_i18n.c 的串表读取（P5 编入真实现；此前把键原样返回，提示语退化为键名）---- */

/* ---- 第二批（链接器逐轮暴露）：diag 的难度/佣兵读数、原版提示、padmode 的其余接口、秘境读数、探路测试开关 ---- */
                     /* 自定义页开关缺省全关（真值在 dm_custom.c，P8d） | custom-page toggles all off by default (real ones in dm_custom.c, P8d) */
                                     /* 不在局内的读数；一键重开用（P8b 接 diag 后给真值） */

uint64_t dm_stick_last_ns(void) { uint64_t dm_input_stick_activity(void); return dm_input_stick_activity(); }   /* 真值：input_backend 记的摇杆活动时刻 | real value from input_backend */
_Atomic int g_dm_seek_tp;
int g_dm_seek_tp_x, g_dm_seek_tp_y;

/* ---- 第三批（padmode 编入后）：符文之语界面重建、选项页模式进入（P8b/P8d）---- */
