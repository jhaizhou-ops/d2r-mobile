// 触屏版点按自动选怪：Boss > 精英 > 普通。
// 原版手柄选目标 main+0x2f70b4 把最多 12 个候选（{类型, 单位号, 分数}，分数越高越优先、初值 -1）逐个交给
// 比较函数 main+0x2f7664(闭包, 单位, 类别, s0=分数)：分数高于当前最佳且过完原版全部资格/射程/视线检查才换成最佳。
// 这里在比较函数入口（观测门 OBS_TARGET_RANK，只在 touch-run-v4 规则域）把怪物候选的分数按档位抬高：
// 同档内仍按原版分数比，跨档高档优先；资格检查一行不动（被拒的高档怪照样被拒）。
// 档位：Boss = monstats.txt boss=1 的类（3.3 数据，见 BOSS_CLASS）；精英 = 怪物数据 TypeFlag 含超级独特/冠军/独特。
// Tap auto-targeting in the touch version: Boss > elite > normal.
// The original controller target selection main+0x2f70b4 hands up to 12 candidates ({type, unit id, score}, higher score = higher priority, initial value -1) one by one to
// the compare function main+0x2f7664(closure, unit, category, s0=score): a candidate becomes the best only if its score beats the current best and it passes all of the original eligibility/range/line-of-sight checks.
// Here, at the compare function's entry (observation gate OBS_TARGET_RANK, only in the touch-run-v4 rule domain), the score of monster candidates is raised by tier:
// within a tier the original scores still decide, across tiers the higher tier wins; the eligibility checks are not touched at all (a rejected high-tier monster is still rejected).
// Tiers: Boss = classes with boss=1 in monstats.txt (3.3 data, see BOSS_CLASS); elite = monster data TypeFlag includes super unique/champion/unique.
#include "dm.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <math.h>

enum { UNIT_MONSTER = 1, TF_SUPERUNIQUE = 0x02, TF_CHAMPION = 0x04, TF_UNIQUE = 0x08 };
// 档位加分须远大于原版分数的量级：真机读数见 DM_TARGET_LOG（分数是 0 附近的小浮点）
// The tier bonus must be far larger than the magnitude of the original game's scores: for real-device readings see DM_TARGET_LOG (scores are small floats near 0)
static const float RANK_BONUS[3] = {0, 1000, 2000};
// data/global/excel/monstats.txt（3.3）boss=1 的 *hcIdx
// *hcIdx of the rows with boss=1 in data/global/excel/monstats.txt (3.3)
static const uint16_t BOSS_CLASS[] = {156, 211, 229, 242, 243, 250, 256, 267, 333, 365, 526, 544, 546, 547, 548, 549, 550,
                                      559, 570, 704, 705, 706, 707, 708, 709, 745, 746, 747};

static int read_live(uint64_t a, void *out, size_t n) {
    vm_size_t got = 0;
    return a >= 0x1000 && a <= UINT64_MAX - n &&
           vm_read_overwrite(mach_task_self(), a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}

// 返回 0 普通 / 1 精英 / 2 Boss；读不到或不是怪物 = -1（不动分数）
// Returns 0 normal / 1 elite / 2 Boss; unreadable or not a monster = -1 (score left alone)
static int unit_rank(uint64_t unit, uint32_t *cls, uint8_t *flags) {
    uint32_t head[2]; uint64_t data = 0;
    // 单位：+0 类型、+4 类号、+0x10 类型数据（怪物 = MonsterData，+0x1A TypeFlag）
    // Unit: +0 type, +4 class id, +0x10 type data (monster = MonsterData, +0x1A TypeFlag)
    if (!read_live(unit, head, sizeof head) || head[0] != UNIT_MONSTER) return -1;
    *cls = head[1];
    for (size_t i = 0; i < sizeof BOSS_CLASS / sizeof *BOSS_CLASS; i++) if (BOSS_CLASS[i] == head[1]) return 2;
    if (!read_live(unit + 0x10, &data, 8) || !read_live(data + 0x1a, flags, 1)) return -1;
    return (*flags & (TF_SUPERUNIQUE | TF_CHAMPION | TF_UNIQUE)) ? 1 : 0;
}
// 越肩镜头的怪物指示（diag.c cam_threats_tick）用同一套档位：0 普通 / 1 精英 / 2 Boss，不是怪 -1
// The over-the-shoulder camera's monster indicators (diag.c cam_threats_tick) use the same tiers: 0 normal / 1 elite / 2 Boss, -1 when not a monster
int dm_target_rank(uint64_t unit) { uint32_t cls = 0; uint8_t flags = 0; return unit_rank(unit, &cls, &flags); }
// 怪物指示的配色档（diag.c cam_threats_tick → dm_cam_threats）：按原版名字颜色分开 0 普通 / 1 冠军（蓝）/ 2 独特与超级独特（暗金）/ 3 幕首领；
// 不是怪 -1。自动选目标仍用上面三档（精英并成一档），这里只多分一层，不改选怪
// Colour tier for the monster indicators (diag.c cam_threats_tick -> dm_cam_threats), split by the original name colours: 0 normal / 1 champion (blue) /
// 2 unique and super unique (gold) / 3 act boss; -1 when not a monster. Auto-targeting keeps the three tiers above (elites merged); this only splits one more
// level and does not change targeting
int dm_threat_tier(uint64_t unit) {
    uint32_t cls = 0; uint8_t flags = 0; int r = unit_rank(unit, &cls, &flags);
    return r < 0 ? -1 : r == 2 ? 3 : (flags & (TF_SUPERUNIQUE | TF_UNIQUE)) ? 2 : (flags & TF_CHAMPION) ? 1 : 0; }

static FILE *g_log; static unsigned g_nlog;
static void target_rank_obs(const uint64_t *regs) {
    // dm_gate 帧：x0..x30 在 regs[0..30]，q0..q31 从 regs[32] 起（entry.S，256 字节处）；改 regs[32] 低 32 位 = 改 s0
    // dm_gate frame: x0..x30 are in regs[0..30], q0..q31 start at regs[32] (entry.S, at byte 256); changing the low 32 bits of regs[32] = changing s0
    float *s0 = (float *)(void *)&regs[32];
    uint32_t cls = 0; uint8_t flags = 0;
    int rank = regs[1] ? unit_rank(regs[1], &cls, &flags) : -1;
    float before = *s0;
    if (rank > 0 && isfinite(before)) *s0 = before + RANK_BONUS[rank];
    if (g_log && g_nlog < 4096) {
        g_nlog++;
        fprintf(g_log, "{\"unit\":\"%#llx\",\"kind\":%u,\"class\":%u,\"flags\":%u,\"rank\":%d,\"score\":%.6g,\"adjusted\":%.6g}\n",
                (unsigned long long)regs[1], (unsigned)regs[2], cls, flags, rank, before, *s0);
        fflush(g_log);
    }
}

void target_rank_install(void) {
    if (!gate_obs_present(OBS_TARGET_RANK)) {          // native 规则域（Mac 对原版真值的闸门）没有这个门：原版选目标不变 | The native rule domain (the Mac gate check against the original game's ground truth) does not have this gate: the original target selection is unchanged
#if DM_PHONE
        LOG("【红】产物没有观测门 OBS_TARGET_RANK：自动选怪（Boss > 精英 > 普通）不工作，退回原版选目标");   // iOS 包一定是 touch 规则域，缺门 = 出错包，不许静默 | An iOS app is always in the touch rule domain; a missing gate = a broken package, must not be silent
#endif
        return;
    }
    const char *lg = getenv("DM_TARGET_LOG");
    if (lg && !(g_log = fopen(lg, "w"))) LOG("【红】DM_TARGET_LOG 打不开 %s", lg);
    gate_obs_set(OBS_TARGET_RANK, target_rank_obs);
    LOG("自动选怪：Boss > 精英 > 普通（观测门 %d）%s%s", OBS_TARGET_RANK, lg ? "，候选记录 → " : "", lg ? lg : "");
}
