// 导入解析（已实现接宿主、其余接记名桩）与宿主门分派。
// Import resolution (implemented ones go to the host, the rest go to named stubs) and host gate dispatch.
#include "dm.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <dlfcn.h>
#include <time.h>

typedef struct { const char *name; void *fn; } hle_ent;

// ---------- 未实现导入：记名桩（entry.S 里静态汇编的桩表，第 k 个 = movz x9,#k; b unimpl_entry）----------
// ---------- Unimplemented imports: named stubs (the statically assembled stub table in entry.S, the k-th = movz x9,#k; b unimpl_entry) ----------
#define MAX_STUB 4096                 // 与 entry.S 的 .rept 次数一致 | Matches the .rept count in entry.S
static unsigned g_nstub;
static const char *g_stub_name[MAX_STUB];
static uint8_t g_stub_seen[MAX_STUB];
extern uint64_t unimpl_stubs[MAX_STUB];   // 每桩 8 字节 | 8 bytes per stub

uint64_t unimpl_c(unsigned idx, uint64_t *regs) {
    if (!g_stub_seen[idx]++)
        LOG("UNIMPL %s (x0=%#llx x1=%#llx x2=%#llx)", g_stub_name[idx], regs[0], regs[1], regs[2]);
    return 0;
}

// 每模块一张符号号 → 已解析地址缓存
// Per module, one cache of symbol number → resolved address
static uint64_t g_cache[MAX_MOD][8192];

uint64_t resolve_import(module_t *m, const char *name, int weak) {
    static const hle_ent *sec; static unsigned long secsz;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &secsz);
    for (unsigned i = 0; i < secsz / sizeof(hle_ent); i++)
        if (!strcmp(sec[i].name, name)) {
            if (g_flag.imports) { Dl_info di; dladdr(sec[i].fn, &di); LOG("import %s -> %s", name, di.dli_sname); }
            void *hlecount_wrap(const char *, const char *, void *); return (uint64_t)hlecount_wrap("HLE", name, sec[i].fn);   // DM_HLECOUNT 时为计数桩（含 libc 直通表） | With DM_HLECOUNT it is a counting stub (including the libc passthrough table)
        }
    // 未解析的弱导入保持零，让客体能检测可选函数缺失。
    // Leave unresolved weak imports null so the guest can detect absent optional functions.
    if (weak) return 0;
    // 已为同名分过桩就复用
    // Reuse the stub if one was already allocated for the same name
    void *hlecount_wrap(const char *, const char *, void *);
    for (unsigned k = 0; k < g_nstub; k++)
        if (!strcmp(g_stub_name[k], name)) return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[k]);
    if (g_nstub >= MAX_STUB) die("stub overflow");
    unsigned k = g_nstub++;
    g_stub_name[k] = name;
    (void)m;
    return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[k]);
}

void stubs_report(void) { LOG("stubs=%u", g_nstub); }

// ---------- 宿主门分派 ----------
// 改写器把每处 svc #n / mrs cntpct 换成调用 dm_gate；这里按门块返回点（绝对地址）记住它原来是什么。
// 查法：开放寻址哈希，一次命中（原先每次门调用 module_of + 线性比对 110 项，且按模块内偏移做键，跨模块会串）。
// ---------- Host gate dispatch ----------
// The rewriter replaces every svc #n / mrs cntpct with a call to dm_gate; here we remember, by gate block return point (absolute address), what it originally was.
// Lookup: open-addressing hash, one hit (it used to do module_of + a linear compare over 110 entries on every gate call, keyed by in-module offset, which would collide across modules).
enum { G_SVC, G_CNTPCT, G_CNTVCT, G_OBS };
// 观测门（改写器 OBS 表离线插入）：编号 → 只读回调；未挂回调时门进出一次即返回
// Observation gates (inserted offline from the rewriter's OBS table): number → read-only callback; with no callback attached the gate returns after one pass in and out
static void (*g_obs_fn[OBS_MAX])(const uint64_t *regs); static int g_obs_have[OBS_MAX];
void gate_obs_set(int id, void (*fn)(const uint64_t *regs)) { if (id <= 0 || id >= OBS_MAX) die("观测门编号越界 %d", id); g_obs_fn[id] = fn; }
int gate_obs_present(int id) { return id > 0 && id < OBS_MAX && g_obs_have[id]; }
static uint8_t g_svc_have[256];   // 产物里登记过的 svc 号（剪口引入的宿主调用号 0x80 起靠它判在不在） | svc numbers registered in the build output (used to tell whether the host call numbers from 0x80 up, introduced by cuts, are present)
int gate_svc_present(int num) { return num >= 0 && num < 256 && g_svc_have[num]; }
typedef struct { uint64_t ret; int kind, arg; uint64_t orig; } gate_t;
#define GATE_BITS 12                     // 4096 槽，门数 ≤ 1024，装载因子 ≤ 1/4 | 4096 slots, gate count ≤ 1024, load factor ≤ 1/4
static gate_t g_gate[1u << GATE_BITS];
static int g_ngate;
static inline unsigned gate_h(uint64_t a) { return (unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - GATE_BITS)); }

void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig) {
    int k = !strcmp(kind, "svc") ? G_SVC : !strcmp(kind, "cntpct") ? G_CNTPCT : !strcmp(kind, "cntvct") ? G_CNTVCT : !strcmp(kind, "obs") ? G_OBS : -1;
    if (k < 0) die("未知门类 %s", kind);
    if (k == G_OBS) { if (arg <= 0 || arg >= OBS_MAX) die("观测门编号越界 %d", arg); g_obs_have[arg] = 1; }
    if (k == G_SVC && arg >= 0 && arg < 256) g_svc_have[arg] = 1;
    if (++g_ngate > (1 << GATE_BITS) / 4) die("gate overflow");
    unsigned i = gate_h(ret_abs);
    while (g_gate[i].ret) { if (g_gate[i].ret == ret_abs) die("门重复登记 %#llx", ret_abs); i = (i + 1) & ((1u << GATE_BITS) - 1); }
    g_gate[i] = (gate_t){ret_abs, k, arg, orig};
}

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);   // hle_svc.c

uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs) {
    for (unsigned i = gate_h(ret_addr);; i = (i + 1) & ((1u << GATE_BITS) - 1)) {
        const gate_t *g = &g_gate[i];
        if (g->ret == ret_addr) switch (g->kind) {
            case G_CNTPCT: case G_CNTVCT: {
                uint64_t t = guest_ns() * 12 / 625;    // 19.2 MHz 系统计数器；原指令 mrs xN, cntpct_el0，arg = N | 19.2 MHz system counter; original instruction mrs xN, cntpct_el0, arg = N
                regs[g->arg] = t;
                return t;
            }
            case G_OBS: if (g_obs_fn[g->arg]) g_obs_fn[g->arg](regs); return 0;   // 只读寄存器；dm_gate 回写的 x0–x8 即原值 | Registers are read-only; the x0–x8 that dm_gate writes back are the original values
            default: return svc_dispatch(g->arg, regs, g->orig);
        }
        if (!g->ret) die("unknown gate at %#llx", ret_addr);
    }
}
