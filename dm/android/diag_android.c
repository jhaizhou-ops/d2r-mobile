/* Android 的 diag 少量读数（P8 需要的那几件；diag.c 整体在安卓不编）：
 *   · dm_request_save：请游戏存一次盘（发处拿 G+0x5800 的计时位，0 = 刚进局不动；diag.c:1196 同一口径）
 *   · dm_quiet_difficulty_msg + 事件门 15：秘境局里拦原版「游戏难度设为 N…」提示（子类型 0x17 改成表外值 0xff）
 *   · dm_prof.c 的两件：libSystem 私有计数接口（Mac/iOS 专有），安卓给中性实现
 * G 由 HUD 抬数那侧的服务端拍门记（一个门只能挂一个回调，所以共用 hud_read_android.c 那一路）。
 * The few diag facilities P8 needs on Android (diag.c itself is not compiled there):
 *   · dm_request_save: ask the game to save once (same convention as diag.c:1196)
 *   · dm_quiet_difficulty_msg + event gate 15: suppress the original 「游戏难度设为 N…」notice during a rift run
 *   · dm_prof.c's two entry points are backed by libSystem private counters (Mac / iOS only); neutral values on Android */
#include "../host/hle.h"
#include "../host/dm.h"
#include <stdatomic.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

/* 「显示物品」键（L）交给游戏的按下次数（hle_hid.c 只在非安卓分支记，安卓分支在 input_backend.c 这里记） */
_Atomic int g_dm_l_presses;
static _Atomic int g_quiet_diff;
void dm_quiet_difficulty_msg(int on) { atomic_store(&g_quiet_diff, on); }
static void event_msg_obs(const uint64_t *regs) {
    uint8_t *pkt = (uint8_t *)(uintptr_t)regs[0];
    if (!atomic_load(&g_quiet_diff) || !pkt || pkt[1] != 0x17) return;
    static int n; if (n++ < 8) LOG("秘境：拦下原版「游戏难度设为 N」提示（事件子类型 0x17）");
    pkt[1] = 0xff;   /* 表外值 → 原版走默认分支不显示；人数在服务端照常生效 */
}

int dm_request_save(void) {
    uint64_t dm_android_hud_last_G(void);
    uint64_t G = dm_android_hud_last_G();
    if (!G) return 0;
    uint64_t last = 0;
    struct iovec local = { &last, 8 }, remote = { (void *)(uintptr_t)(G + 0x5800), 8 };
    if (syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) != 8 || !last) return 0;
    *(volatile uint64_t *)(uintptr_t)(G + 0x5800) = 1;
    return 1;
}

/* dm_prof.c（libSystem 私有指令 / 周期计数）与面板查找自检：安卓不编真实现，中性即可 */
int dm_ui_top_chk_on(void) { return 0; }
void dm_ui_top_chk(uint64_t root, const char *name, uint64_t got) { (void)root; (void)name; (void)got; }
int dm_ipf_on(void) { return 0; }
uint64_t dm_ipf_instr(void) { return 0; }
void dm_ipf_add(int table, const char *name, uint64_t instr) { (void)table; (void)name; (void)instr; }

void dm_android_diag_install(void) {
    if (gate_obs_present(OBS_EVENT_MSG)) gate_obs_set(OBS_EVENT_MSG, event_msg_obs);
}