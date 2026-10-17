// 宿主公共杂项：客体对象旁表、单调时钟、modules 都没定义的 C 库导入。
// （原 nn::os/mem/lmem/C++ ABI 的 136 条 HLE 已删：SDK 自带 nn::os，装载时这些条目一条都没被解析用上。）
#include <pthread.h>
#include "hle_ohos_compat.h"
#include <time.h>
#include <sys/random.h>
#include <stdatomic.h>

// ---------- 旁表 ----------
#define OM_SIZE (1u << 18)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & (OM_SIZE - 1); }
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1))
        if (g_om[i].k == k || !g_om[i].k || g_om[i].k == (void *)1) { g_om[i].k = k; g_om[i].v = v; break; }
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; g_om[i].k = (void *)1; g_om[i].v = NULL; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) { struct timespec ts; if(clock_gettime(CLOCK_MONOTONIC,&ts))die("clock_gettime"); return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }
// 客体时钟：客体能读到的系统计数器（mrs cntpct 门、svc GetSystemTick）都经这里。缺省 = now_ns()。
// 冻结（只供测试驱动 SW_TICK_HOLD 用）：冻结期间客体时钟不走 ⇒ 游戏不会到期下一拍；解冻后扣掉冻结时长，客体时钟连续不跳
// SW_DETCLOCK=<步长 ns>（回放 / 测试局用；产品局不设 = 墙钟）：首次出图起客体时钟改为「每次出图推进固定步长」，不跟墙钟——
// 客户端每帧 dt、服务端拍的补拍快照都由出图次数决定，同一回放两遍局内第 N 帧逐字节可比（10-02 总指挥：画面逐字节证明的地基）。
// 首帧之前仍走墙钟（启动期有按时刻轮询的等待，冻住会卡）；切换时从当时的客体时钟值接续，不跳。冻结期间不推进。
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_guest_frame; static _Atomic int g_gfz, g_gdet_on; static uint64_t g_gdet_step = 1;
uint64_t sw_guest_frame(void) { return atomic_load(&g_guest_frame); }
static int detclock_cfg(void) {
    static int c = -1; if (c < 0) { const char *e = getenv("SW_DETCLOCK"); g_gdet_step = e ? strtoull(e, 0, 0) : 0; c = g_gdet_step > 0; }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) { return atomic_load(&g_gdet_on) ? atomic_load(&g_gdet) : wall_guest_ns(); }
int sw_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
// 进局对齐：前端 / 选角阶段出图帧数随真实时间变（装载在后台线程按墙钟推进），首帧起累计的客体时钟值因此每局不同——
// 依赖绝对时刻的画面（如生命 / 法力球液面）在同拍仍不同（10-02 实测拍 E + 2 帧两遍差约 1.8 万像素，集中在两侧球位）。
// 见到 UI 消息 FrontEndNavigation:ToGame（建局、服务端拍时限初始化之前）即把客体时钟设为固定值 2^52 ns（只许前跳），
// 此后纯按出图推进：拍 E 落在第几帧、那一刻的绝对时刻都只由步长决定。
#define DET_TOGAME_NS (1ull << 52)
static void det_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)arg; if (strcmp(cat, "FrontEndNavigation") || strcmp(act, "ToGame")) return;
    uint64_t cur = atomic_load(&g_gdet);
    if (cur >= DET_TOGAME_NS) { LOG("【红】SW_DETCLOCK：进局时客体时钟 %llu ≥ 对齐值 2^52，不回拨（对齐未生效）", (unsigned long long)cur); return; }
    atomic_store(&g_gdet, DET_TOGAME_NS);
    LOG("SW_DETCLOCK：进局（ToGame）客体时钟对齐到 2^52 ns（前跳 %.3f s）", (DET_TOGAME_NS - cur) / 1e9);
}
void sw_guest_frame_tick(void) {
    crash_watch_frame();
    atomic_fetch_add(&g_guest_frame, 1);
    if (!detclock_cfg()) return;
    if (!atomic_load(&g_gdet_on)) {
        atomic_store(&g_gdet, wall_guest_ns()); atomic_store(&g_gdet_on, 1);
        void uimsg_subscribe(void (*)(const char *, const char *, const char *)); uimsg_subscribe(det_on_uimsg);
        LOG("SW_DETCLOCK：首次出图起客体时钟按出图推进，步长 %llu ns；进局时对齐到 2^52 ns", (unsigned long long)g_gdet_step); return;
    }
    if (!atomic_load(&g_gfz)) { atomic_fetch_add(&g_gdet, g_gdet_step); sw_guest_clock_advanced(); }
}
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) { atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag)); atomic_store(&g_gfz, 1); }
    else if (!on && atomic_load(&g_gfz)) { atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen)); atomic_store(&g_gfz, 0); }
}
// 冻结有几个互不相干的原因（0 测试用的冻结拍 / 读取画面，1 App 在后台，2 系统键盘弹着）：任一成立就冻结，全部解除才放行——
// 否则「键盘弹着时切后台再回来」会被回前台那次解冻提前放开
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER; static unsigned g_gfz_reasons;
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason; else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze(int on) { guest_clock_freeze_for(0, on); }
// 熵源：缺省真随机（arc4random）；SW_DETCLOCK 开时改为固定种子的 splitmix64 流（回放画面逐字节可比用，10-02：营地蓝火粒子两遍不同）。
// 两处出口（libc getentropy、svc GetInfo RandomEntropy）共用一条流；调用次数在日志里报（首次与每 1000 次）
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull; static _Atomic uint64_t g_detrng_n;
uint64_t sw_entropy64(void) {
    if (!detclock_cfg()) { uint64_t v; if(getrandom(&v,8,0)!=8)die("getrandom"); return v; }
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1; if (n == 1 || n % 1000 == 0) LOG("SW_DETCLOCK：确定熵第 %llu 次", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) return getrandom(p,n,0)==(ssize_t)n ? 0 : -1;
    for (size_t i = 0; i < n; i += 8) { uint64_t v = sw_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
