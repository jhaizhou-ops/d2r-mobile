// Granny 段解码（BitKnit2，格式 4）的宿主门与预解码（方案 A）。
// 剪口：段读取两条流式路径里「逐块增量解码」的调用点 bl 0x11f4af0 改成 svc
//   0x11f02dc（段读取 0x11f0110：x24 = 压缩长度，x28 = 本块之前已读字节）→ svc #0x83
//   0x11f0914（段读取 0x11f07d0：x25 = 压缩长度，x26 = 本块之前已读字节）→ svc #0x84
// 两处的块都是「压缩数据按 0x2000 切、末块取余」，块放在调用方栈上的缓冲区；循环体在块之间不读解码器上下文（ctx）与输出缓冲。
//
// 解码器上下文（0x11f29d0 建，64 字节对齐，分配 0x3f57 字节 ⇒ ctx 起至少 0x3f18 字节可用）：
//   +0x00 输出起点 / +0x08 输出当前 / +0x10 输出缓冲 / +0x18 输出终点（四个指针，均指进输出缓冲）
//   +0x20..0x3f 四组 0x100000001，+0x40 = 1，+0x44 = 0xfac688，+0x48 状态（>0 解码中、0 结束、<0 出错），+0x4c = 0x10000，+0x50 = 0x10000，
//   +0x58 暂存尾长，+0x60 暂存尾（0x20），+0x80 已消耗输入，+0x88 起模型表（0x11f4c70 初始化）
// 预解码：L3' 预取线程解出的文件内容以 gr2 文件头开头 ⇒ 解析段表，把格式 4 段的压缩数据拷进候选表，后台线程用游戏自己的
//   0x11f4c70 + 0x11f4af0 在宿主内存里按同样的 0x2000 切法预解（纯计算、不碰分配器与线程区以外的游戏状态）。
// 替换（缺省，DM_GRANNY_PRE=0 关）：游戏第一块到来时按（压缩长度、首块内容）找已解完的候选；之后每块逐字节核对压缩数据（推迟真解码），
//   末块核过即把解压结果与 ctx 终态整体搬入（四个指针按输出缓冲地址重定位，ctx 只搬「解码会写」的字节，见 DM_GRANNY_SELFTEST）。
//   任何一块对不上 ⇒ 把已推迟的块按原切法补喂真解码器，再照常解当前块（结果与从未替换相同）。
//   等价依据：压缩字节逐字节相同 + 同一解码代码 + 同一切法 ⇒ 输出与 ctx 终态逐字节相同；DM_GRANNY_VERIFY=1 整局实测比对。
//   已知不覆盖：读块失败（读到的字节数不等于请求）时游戏提前出循环，推迟中的 ctx 会停在初态 —— 只在 I/O 出错时发生，此时段读取本身已失败。
// DM_GRANNY_PROF=1（需 DM_BOOTPROF=1 定选角时刻）：按「启动 → 选角主菜单」「选角之后」两段、按客体线程累计调用数、输入字节、耗时。
// Host gate and pre-decoding (plan A) for Granny segment decoding (BitKnit2, format 4).
// Cut: in the two streaming paths of segment reading, the call site of "incremental per-chunk decoding", bl 0x11f4af0, is changed to svc
//   0x11f02dc (segment read 0x11f0110: x24 = compressed length, x28 = bytes read before this chunk) → svc #0x83
//   0x11f0914 (segment read 0x11f07d0: x25 = compressed length, x26 = bytes read before this chunk) → svc #0x84
// In both places the chunks are "compressed data split at 0x2000, the last chunk takes the remainder", and the chunk sits in a buffer on the caller's stack; between chunks the loop body reads neither the decoder context (ctx) nor the output buffer.
// Decoder context (built by 0x11f29d0, 64-byte aligned, allocation 0x3f57 bytes ⇒ at least 0x3f18 bytes usable from the start of ctx):
//   +0x00 output start / +0x08 output current / +0x10 output buffer / +0x18 output end (four pointers, all pointing into the output buffer)
//   +0x20..0x3f four groups of 0x100000001, +0x40 = 1, +0x44 = 0xfac688, +0x48 state (>0 decoding, 0 finished, <0 error), +0x4c = 0x10000, +0x50 = 0x10000,
//   +0x58 stashed tail length, +0x60 stashed tail (0x20), +0x80 input consumed, from +0x88 the model tables (initialized by 0x11f4c70)
// Pre-decoding: file contents decoded by the L3' prefetch thread start with a gr2 file header ⇒ parse the segment table, copy the compressed data of format 4 segments into the candidate table, and a background thread uses the game's own
//   0x11f4c70 + 0x11f4af0 to pre-decode in host memory with the same 0x2000 split (pure computation, touching no game state other than the allocator and the thread area).
// Replacement (default, DM_GRANNY_PRE=0 turns it off): when the game's first chunk arrives, look up a finished candidate by (compressed length, first chunk contents); after that each chunk's compressed data is checked byte by byte (the real decode is deferred),
//   once the last chunk has been checked, the decompressed result and the ctx final state are moved in as a whole (the four pointers relocated to the output buffer address, of ctx only the "written by decoding" bytes are moved, see DM_GRANNY_SELFTEST).
//   Any chunk that mismatches ⇒ feed the deferred chunks to the real decoder with the original split, then decode the current chunk as usual (the result is the same as never having replaced).
//   Basis for equivalence: compressed bytes byte-for-byte identical + same decoding code + same split ⇒ output and ctx final state byte-for-byte identical; DM_GRANNY_VERIFY=1 compares over a whole run by measurement.
//   Known not covered: when a chunk read fails (bytes read not equal to bytes requested) the game leaves the loop early and the deferred ctx stays in its initial state -- this only happens on an I/O error, in which case the segment read itself has already failed.
// DM_GRANNY_PROF=1 (needs DM_BOOTPROF=1 to fix the character select moment): accumulates call count, input bytes and time per guest thread, in two phases "boot → character select main menu" and "after character select".
#include "hle.h"
#include "dm_alloc.h"
#include <pthread/qos.h>
#include <stdatomic.h>
uint32_t thr_cur_handle(void);

#define G_CHUNK 0x2000u
#define G_CTX 0x3f18u                     // 可安全读写的 ctx 长度（分配 0x3f57 − 最大对齐垫 0x3f） | ctx length that is safe to read and write (allocation 0x3f57 − maximum alignment padding 0x3f)
static uint64_t (*g_dec)(uint64_t, uint64_t, uint64_t);   // 0x11f4af0(ctx, 块, 长度) → bool | 0x11f4af0(ctx, chunk, length) → bool
static void (*g_init)(uint64_t, uint64_t);                // 0x11f4c70(ctx + 0x88, 0)
static void g_fns(void) {
    if (g_dec) return;
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    g_init = (void (*)(uint64_t, uint64_t))(m->base + 0x11f4c70);
    g_dec = (uint64_t (*)(uint64_t, uint64_t, uint64_t))(m->base + 0x11f4af0);
}

// ---------- 计时（DM_GRANNY_PROF）----------
// ---------- Timing (DM_GRANNY_PROF) ----------
#define GP_THR 16
static struct { uint32_t h; uint64_t n, in, ns; } g_gp[2][GP_THR];
static int g_gprof = -1, g_gphase;
static pthread_mutex_t g_gpl = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_gpseg[2][2], g_gpdl[2][2];   // [段][路径 A/B]：段数、解压字节（首块时按 ctx +0x18 − +0x10 计） | [phase][path A/B]: segment count, decompressed bytes (counted as ctx +0x18 − +0x10 at the first chunk)
static void gp_dump(int ph) {
    static const char *nm[2] = {"启动 → 选角主菜单", "选角之后"};
    uint64_t n = 0, in = 0, ns = 0;
    for (int i = 0; i < GP_THR && g_gp[ph][i].h; i++) { n += g_gp[ph][i].n; in += g_gp[ph][i].in; ns += g_gp[ph][i].ns; }
    LOG("Granny 解码（%s）：%llu 块、压缩 %.1f MiB、%.3f 线程·秒；段 A %llu / B %llu，解压 A %.1f / B %.1f MiB", nm[ph], (unsigned long long)n, in / 1048576.0, ns / 1e9,
        (unsigned long long)g_gpseg[ph][0], (unsigned long long)g_gpseg[ph][1], g_gpdl[ph][0] / 1048576.0, g_gpdl[ph][1] / 1048576.0);
    for (int i = 0; i < GP_THR && g_gp[ph][i].h; i++)
        LOG("  线程 %#x：%llu 块、%.1f MiB、%.3f 秒", g_gp[ph][i].h, (unsigned long long)g_gp[ph][i].n, g_gp[ph][i].in / 1048576.0, g_gp[ph][i].ns / 1e9);
}
static void gp_add(uint64_t t0, uint64_t len) {
    uint64_t dt = now_ns() - t0, rib = dm_prof_ribbon_ns(); int ph = rib && t0 >= rib;
    uint32_t h = thr_cur_handle();
    pthread_mutex_lock(&g_gpl);
    if (ph != g_gphase) { gp_dump(g_gphase); g_gphase = ph; }
    for (int i = 0; i < GP_THR; i++)
        if (g_gp[ph][i].h == h || !g_gp[ph][i].h) { g_gp[ph][i].h = h; g_gp[ph][i].n++; g_gp[ph][i].in += len; g_gp[ph][i].ns += dt; break; }
    pthread_mutex_unlock(&g_gpl);
}

// ---------- 候选表 ----------
// ---------- Candidate table ----------
enum { GS_PENDING, GS_BUSY, GS_READY, GS_GONE };
typedef struct gseg {
    uint32_t clen, dlen, olen; uint8_t *cmp, *out, *ctx, *wm; uint64_t ctxbase;   // wm：本段 ctx「解码会写」位图（两种填充下终值相同的字节）   // olen：解出字节（终态 +0x08 − 起点）；ctxbase：预解时 ctx 地址 | wm: this segment's ctx "written by decoding" bitmap (bytes whose final value is the same under both fills)   // olen: decoded bytes (final state +0x08 − start); ctxbase: ctx address at pre-decode time
    _Atomic int st; struct gseg *hnext, *qnext;
} gseg;
#define GH_BITS 14
static gseg *g_gh[1u << GH_BITS], *g_qhead, **g_qtail = &g_qhead;
static pthread_mutex_t g_gl = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_gc = PTHREAD_COND_INITIALIZER;
static int g_gmode = -1;                  // 0 关 / 1 替换 / 2 校验（DM_GRANNY_VERIFY） | 0 off / 1 replace / 2 verify (DM_GRANNY_VERIFY)
static int g_gself;                       // DM_GRANNY_SELFTEST
static _Atomic int g_gstop; static _Atomic int64_t g_gout, g_gpeak; static int64_t g_gcap;
static _Atomic uint32_t g_nseg, g_ndone, g_nfail, g_hit, g_wait, g_miss, g_claim, g_fallback, g_vok, g_vbad;

static _Atomic uint32_t g_vwhy[3]; static _Atomic uint32_t g_voff[16];   // 不一致原因：输出 / ctx 会写字节 / 指针；ctx 首个不同偏移按 0x400 分桶 | mismatch reason: output / ctx written bytes / pointers; first differing ctx offset bucketed by 0x400
static unsigned gh_hash(uint32_t clen, const uint8_t *p, uint32_t n) {
    uint64_t h = clen * 0x9E3779B97F4A7C15ull; for (uint32_t i = 0; i < n && i < 64; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return (unsigned)(h >> (64 - GH_BITS));
}
static void gs_free(gseg *s) {            // 持 g_gl | holds g_gl
    if (s->out) atomic_fetch_sub(&g_gout, (int64_t)s->dlen + G_CTX);
    free(s->out); dm_aligned_free(s->ctx); free(s->cmp); free(s->wm); s->out = s->ctx = s->cmp = s->wm = NULL;
    atomic_store(&s->st, GS_GONE); pthread_cond_broadcast(&g_gc);
}

// 在宿主内存里照 0x11f29d0 建 ctx 并按游戏切法解完；成功返回 1（out / ctx 留给调用方）
// Build the ctx in host memory the way 0x11f29d0 does and decode to the end with the game's split; returns 1 on success (out / ctx are left to the caller)
static int pre_decode(const uint8_t *cmp, uint32_t clen, uint32_t dlen, uint8_t fill, uint8_t **pout, uint8_t **pctx, uint32_t *olen) {
    uint8_t *ctx = dm_aligned_alloc(64, (G_CTX + 63) & ~63u), *out = malloc(dlen ? dlen : 1);
    if (!ctx || !out) { dm_aligned_free(ctx); free(out); return 0; }
    memset(ctx, fill, G_CTX);
    uint64_t o = (uint64_t)out, one = 0x100000001ull;
    memcpy(ctx + 0x00, &o, 8); memcpy(ctx + 0x08, &o, 8); memcpy(ctx + 0x10, &o, 8); o += dlen; memcpy(ctx + 0x18, &o, 8);
    for (int k = 0; k < 4; k++) memcpy(ctx + 0x20 + 8 * k, &one, 8);
    uint64_t v48 = 0x0001000000000001ull; memcpy(ctx + 0x48, &v48, 8);
    uint32_t v = 0x10000; memcpy(ctx + 0x50, &v, 4); v = 1; memcpy(ctx + 0x40, &v, 4);
    g_init((uint64_t)ctx + 0x88, 0);
    v = 0xfac688; memcpy(ctx + 0x44, &v, 4); uint64_t z = 0; memcpy(ctx + 0x58, &z, 8); memcpy(ctx + 0x80, &z, 8);
    for (uint32_t pos = 0; pos < clen; pos += G_CHUNK) {
        uint32_t n = clen - pos < G_CHUNK ? clen - pos : G_CHUNK;
        if (!(g_dec((uint64_t)ctx, (uint64_t)(cmp + pos), n) & 1)) { dm_aligned_free(ctx); free(out); return 0; }
    }
    uint64_t cur; memcpy(&cur, ctx + 0x08, 8);
    if (cur < (uint64_t)out || cur > (uint64_t)out + dlen) { dm_aligned_free(ctx); free(out); return 0; }
    *olen = (uint32_t)(cur - (uint64_t)out); *pout = out; *pctx = ctx; return 1;
}

// DM_GRANNY_SELFTEST：同一段在不同地址、不同填充（0x00 / 0xff）下各解一遍 ⇒ 输出须同；ctx 除四个指针（按输出地址重定位后须同）外，
// 两遍不同的字节 = 解码从不写（保留分配器原值）；记并集与「会写」字节的最大偏移，供替换时只搬会写的字节
// DM_GRANNY_SELFTEST: the same segment is decoded once each at different addresses and with different fills (0x00 / 0xff) ⇒ the outputs must be identical; in ctx, apart from the four pointers (which must be identical after relocation to the output address),
// bytes that differ between the two passes = never written by decoding (allocator's original values kept); record the union and the maximum offset of the "written" bytes, so that replacement moves only the written bytes
static uint8_t g_st_unw[G_CTX]; static _Atomic uint32_t g_st_n, g_st_bad; static uint32_t g_st_maxw;
static void selftest_one(gseg *s) {
    uint8_t *o1, *c1, *o2, *c2; uint32_t l1, l2;
    if (!pre_decode(s->cmp, s->clen, s->dlen, 0x00, &o1, &c1, &l1)) return;
    if (!pre_decode(s->cmp, s->clen, s->dlen, 0xff, &o2, &c2, &l2)) { free(o1); dm_aligned_free(c1); return; }
    int bad = l1 != l2 || memcmp(o1, o2, l1);
    for (int k = 0; k < 4; k++) { uint64_t a, b; memcpy(&a, c1 + 8 * k, 8); memcpy(&b, c2 + 8 * k, 8); if (a - (uint64_t)o1 != b - (uint64_t)o2) bad = 1; }
    pthread_mutex_lock(&g_gl);
    for (uint32_t i = 0x20; i < G_CTX; i++) { if (c1[i] != c2[i]) g_st_unw[i] = 1; else if (i > g_st_maxw) g_st_maxw = i; }
    pthread_mutex_unlock(&g_gl);
    atomic_fetch_add(&g_st_n, 1); if (bad) atomic_fetch_add(&g_st_bad, 1);
    free(o1); dm_aligned_free(c1); free(o2); dm_aligned_free(c2);
}
static void selftest_report(void) {
    uint32_t nu = 0, lo = 0, hi = 0; for (uint32_t i = 0x20; i < G_CTX; i++) if (g_st_unw[i]) { if (!nu++) lo = i; hi = i; }
    LOG("DM_GRANNY_SELFTEST：%u 段双解，输出或指针不一致 %u 段；ctx 从不写的字节 %u 个（%#x–%#x），会写字节最大偏移 %#x",
        atomic_load(&g_st_n), atomic_load(&g_st_bad), nu, lo, hi, g_st_maxw);
    char buf[2048]; int p = 0;
    for (uint32_t i = 0x20; i < G_CTX && p < (int)sizeof buf - 32; i++)
        if (g_st_unw[i] && (i == 0x20 || !g_st_unw[i - 1])) { uint32_t j = i; while (j + 1 < G_CTX && g_st_unw[j + 1]) j++; p += snprintf(buf + p, sizeof buf - p, " %#x-%#x", i, j); }
    LOG("  从不写的区间：%s", p ? buf : "无");
}

static void *g_worker(void *arg) {
    (void)arg; pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    for (;;) {
        pthread_mutex_lock(&g_gl);
        while (!g_qhead && !atomic_load(&g_gstop)) pthread_cond_wait(&g_gc, &g_gl);
        if (atomic_load(&g_gstop)) { pthread_mutex_unlock(&g_gl); break; }
        gseg *s = g_qhead; g_qhead = s->qnext; if (!g_qhead) g_qtail = &g_qhead;
        while (!atomic_load(&g_gstop) && atomic_load(&g_gout) > 0 && atomic_load(&g_gout) + s->dlen + G_CTX > g_gcap) pthread_cond_wait(&g_gc, &g_gl);
        int ex = GS_PENDING; int mine = !atomic_load(&g_gstop) && atomic_compare_exchange_strong(&s->st, &ex, GS_BUSY);
        pthread_mutex_unlock(&g_gl);
        if (!mine) continue;
        if (g_gself) selftest_one(s);
        // 两种填充各解一遍：输出须同；ctx 两遍相同的字节 = 解码会写（各段不同：自检 1,702 段，从不写的区间随数据变），
        // 搬入时只搬这些字节，其余保留游戏分配器给的原值 ⇒ 整块 ctx 与从未替换逐字节相同
        // Decode once with each of the two fills: the outputs must be identical; ctx bytes identical in both passes = written by decoding (differs per segment: self-test of 1,702 segments, the never-written ranges vary with the data),
        // when moving in, only these bytes are moved, the rest keep the original values given by the game allocator ⇒ the whole ctx block is byte-for-byte identical to never having been replaced
        uint8_t *out, *ctx, *o2 = NULL, *c2 = NULL, *wm = NULL; uint32_t olen, l2;
        int ok = pre_decode(s->cmp, s->clen, s->dlen, 0x00, &out, &ctx, &olen);
        if (ok && !(ok = pre_decode(s->cmp, s->clen, s->dlen, 0xff, &o2, &c2, &l2))) { free(out); dm_aligned_free(ctx); }
        if (ok) {
            ok = l2 == olen && !memcmp(out, o2, olen) && (wm = calloc(G_CTX / 8 + 1, 1));
            if (ok) for (uint32_t i = 0x20; i < G_CTX; i++) if (ctx[i] == c2[i]) wm[i >> 3] |= 1 << (i & 7);
            free(o2); dm_aligned_free(c2);
            if (!ok) { free(out); dm_aligned_free(ctx); free(wm); }
        }
        pthread_mutex_lock(&g_gl);
        if (ok && !atomic_load(&g_gstop)) {
            s->out = out; s->ctx = ctx; s->wm = wm; s->olen = olen; s->ctxbase = (uint64_t)ctx;
            int64_t v = atomic_fetch_add(&g_gout, (int64_t)s->dlen + G_CTX) + s->dlen + G_CTX;
            for (int64_t pk = atomic_load(&g_gpeak); v > pk && !atomic_compare_exchange_weak(&g_gpeak, &pk, v);) {}
            atomic_store(&s->st, GS_READY); atomic_fetch_add(&g_ndone, 1);
        } else { if (ok) { free(out); dm_aligned_free(ctx); free(wm); } else atomic_fetch_add(&g_nfail, 1); free(s->cmp); s->cmp = NULL; atomic_store(&s->st, GS_GONE); }
        pthread_cond_broadcast(&g_gc); pthread_mutex_unlock(&g_gl);
    }
    return NULL;
}

static void granny_init(void) {
    if (g_gmode >= 0) return;
    const char *e = getenv("DM_GRANNY_PRE"), *v = getenv("DM_GRANNY_VERIFY"), *mb = getenv("DM_GRANNY_PRE_MB");
    g_gmode = v ? 2 : (e && !strcmp(e, "0")) ? 0 : 1;
    g_gself = getenv("DM_GRANNY_SELFTEST") != NULL;
    g_gcap = (mb ? atoll(mb) : 192) << 20;
    const char *nt = getenv("DM_GRANNY_THREADS"); int th = nt ? atoi(nt) : 2;
    if (g_gmode) { g_fns(); for (int i = 0; i < th; i++) { pthread_t t; if (pthread_create(&t, NULL, g_worker, NULL)) die("Granny 预解码起线程失败"); pthread_detach(t); }
                   LOG("Granny 预解码：%s（%d 线程，缓存封顶 %lld MiB%s）", g_gmode == 2 ? "校验模式 DM_GRANNY_VERIFY" : "替换", th, (long long)(g_gcap >> 20), g_gself ? "，SELFTEST 双解" : ""); }
}

static void g_add(const uint8_t *d, size_t n, uint64_t off, uint32_t clen, uint32_t dlen) {
    if (!clen || off + clen > n) return;
    gseg *s = calloc(1, sizeof *s); if (!s) return;
    s->clen = clen; s->dlen = dlen; s->cmp = malloc(clen); if (!s->cmp) { free(s); return; }
    memcpy(s->cmp, d + off, clen);
    unsigned h = gh_hash(s->clen, s->cmp, s->clen < G_CHUNK ? s->clen : G_CHUNK);
    pthread_mutex_lock(&g_gl);
    s->hnext = g_gh[h]; g_gh[h] = s; *g_qtail = s; g_qtail = &s->qnext;
    pthread_cond_broadcast(&g_gc); pthread_mutex_unlock(&g_gl);
    atomic_fetch_add(&g_nseg, 1);
}
// 预取线程解出一个文件后调：gr2 文件头 ⇒ 把格式 4 段入候选表与工作队列（拷压缩数据；预取缓存随后会释放）
// Called by the prefetch thread after it has decoded a file: gr2 file header ⇒ put the format 4 segments into the candidate table and the work queue (copy the compressed data; the prefetch cache is released afterwards)
void granny_offer(const uint8_t *d, size_t n) {
    if (g_gmode < 0) granny_init();
    if (!g_gmode || atomic_load(&g_gstop) || n < 0x40 || memcmp(d, "\xe5\x9b\x49\x5e", 4)) return;
    uint32_t secoff, nsec; memcpy(&secoff, d + 0x2c, 4); memcpy(&nsec, d + 0x30, 4);
    uint64_t base = 0x20 + (uint64_t)secoff;
    if (nsec > 4096 || base + (uint64_t)nsec * 0x2c > n) return;
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t f[11]; memcpy(f, d + base + 0x2c * i, sizeof f);   // 格式、数据偏移、压缩长、解压长、对齐、First16、First8、指针修正表偏移/条数、混合编组表偏移/条数 | format, data offset, compressed length, decompressed length, alignment, First16, First8, pointer fixup table offset/count, mixed marshalling table offset/count
        if (f[0] != 4) continue;
        g_add(d, n, f[1], f[2], f[3]);                               // 段数据（0x11f0110：从数据偏移起压缩长字节） | segment data (0x11f0110: compressed-length bytes starting at the data offset)
        uint32_t cl;                                                 // 修正表（0x11f07d0：偏移处 4 字节压缩长，其后压缩数据；解压长 = 条数 × 12 / 16） | fixup table (0x11f07d0: 4-byte compressed length at the offset, followed by the compressed data; decompressed length = entry count × 12 / 16)
        if (f[8] && (uint64_t)f[7] + 4 <= n) { memcpy(&cl, d + f[7], 4); g_add(d, n, f[7] + 4, cl, f[8] * 12); }
        if (f[10] && (uint64_t)f[9] + 4 <= n) { memcpy(&cl, d + f[9], 4); g_add(d, n, f[9] + 4, cl, f[10] * 16); }
    }
}

void granny_finish(const char *why) {     // 到选角（与预取同时）/ 优雅退出：停后台、释放、报账（只做一次） | on reaching character select (at the same time as prefetch) / graceful exit: stop the background, release, report the totals (done only once)
    if (g_gprof == 1 && !strcmp(why, "到选角")) { pthread_mutex_lock(&g_gpl); if (g_gphase == 0) { gp_dump(0); g_gphase = 1; } pthread_mutex_unlock(&g_gpl); }   // 启动段计时到选角即报（A/B 局不等退出） | boot-phase timing is reported as soon as character select is reached (A/B runs do not wait for exit)
    static _Atomic int once; if (g_gmode <= 0 || atomic_exchange(&once, 1)) return;
    pthread_mutex_lock(&g_gl); atomic_store(&g_gstop, 1); uint32_t left = 0;
    for (unsigned h = 0; h < (1u << GH_BITS); h++)
        for (gseg *s = g_gh[h]; s; s = s->hnext) {   // GONE 的可能正被游戏线程推迟核对（缓冲归它）：不碰；BUSY 的后台解完见 g_gstop 自己丢 | a GONE one may be under deferred checking by the game thread (the buffer belongs to it): do not touch; a BUSY one is dropped by the background itself when it finishes decoding and sees g_gstop
            int st = atomic_load(&s->st); if (st == GS_READY) left++;
            if (st == GS_READY || st == GS_PENDING) gs_free(s);
        }
    pthread_cond_broadcast(&g_gc); pthread_mutex_unlock(&g_gl);
    LOG("Granny 预解码（%s）：候选 %u 段、后台解完 %u、失败 %u；命中 %u（后台正在解而当场解 %u 次）、未命中 %u（认领 %u）、补喂回退 %u；未取走 %u 段已释放；缓存峰值 %.1f MiB",
        why, atomic_load(&g_nseg), atomic_load(&g_ndone), atomic_load(&g_nfail), atomic_load(&g_hit), atomic_load(&g_wait),
        atomic_load(&g_miss), atomic_load(&g_claim), atomic_load(&g_fallback), left, atomic_load(&g_gpeak) / 1048576.0);
    if (g_gmode == 2) {
        LOG("DM_GRANNY_VERIFY：逐段比对 一致 %u、不一致 %u（输出不同 %u、ctx 会写字节不同 %u、指针不同 %u）", atomic_load(&g_vok), atomic_load(&g_vbad),
            atomic_load(&g_vwhy[0]), atomic_load(&g_vwhy[1]), atomic_load(&g_vwhy[2]));
        for (int k = 0; k < 16; k++) if (atomic_load(&g_voff[k])) LOG("  ctx 首个不同偏移 %#x–%#x：%u 段", k << 10, (k << 10) + 0x3ff, atomic_load(&g_voff[k]));
    }
    if (g_gself) selftest_report();
}
void granny_prof_dump(void) {             // 退出时（dm_host_shutdown） | at exit (dm_host_shutdown)
    granny_finish("退出");
    if (g_gprof != 1) return;
    pthread_mutex_lock(&g_gpl); gp_dump(g_gphase); pthread_mutex_unlock(&g_gpl);
}

// ---------- 游戏侧：svc #0x83 / #0x84 ----------
// ---------- Game side: svc #0x83 / #0x84 ----------
static __thread struct { uint64_t ctx, dst; gseg *c; uint32_t ver; } t_g;   // dst：首块时 ctx +0x10（输出缓冲起点；解码中该字段会前移）   // 本线程正在推迟的那一段（段读取循环不会在同一线程交错） | dst: ctx +0x10 at the first chunk (start of the output buffer; this field moves forward during decoding)   // the segment this thread is currently deferring (the segment read loop does not interleave on the same thread)
static gseg *g_lookup(uint32_t clen, const uint8_t *buf, uint32_t len) {   // 返回已认领（BUSY→等完 / READY）的候选，持有权归调用方 | returns the claimed candidate (BUSY→wait until done / READY), ownership goes to the caller
    unsigned h = gh_hash(clen, buf, len);
    pthread_mutex_lock(&g_gl);
    for (gseg *s = g_gh[h]; s; s = s->hnext) {
        if (s->clen != clen || !s->cmp || memcmp(s->cmp, buf, len)) continue;
        int st = atomic_load(&s->st);
        if (st == GS_GONE) continue;
        if (st == GS_PENDING) { atomic_store(&s->st, GS_GONE); free(s->cmp); s->cmp = NULL; atomic_fetch_add(&g_claim, 1); break; }   // 还没轮到：认领作废，当场解 | its turn has not come yet: claim and invalidate it, decode on the spot
        if (st == GS_BUSY) { atomic_fetch_add(&g_wait, 1); break; }   // 后台正在解（每段解两遍）：不等，当场解（实测等 4 次共 0.249 s，比自己解还慢） | the background is decoding it (each segment is decoded twice): do not wait, decode on the spot (measured: 4 waits totalling 0.249 s, slower than decoding it ourselves)
        if (atomic_load(&s->st) == GS_READY) { atomic_store(&s->st, GS_GONE); pthread_mutex_unlock(&g_gl); return s; }   // GONE 但缓冲留给调用方 | GONE, but the buffer is left to the caller
    }
    pthread_mutex_unlock(&g_gl); return NULL;
}
static void g_release(gseg *s) { pthread_mutex_lock(&g_gl); atomic_store(&s->st, GS_READY); gs_free(s); pthread_mutex_unlock(&g_gl); }
static void g_feed(uint64_t ctx, const uint8_t *p, uint32_t n) {   // 按游戏切法把 p[0, n) 补喂真解码器 | feed p[0, n) to the real decoder, split the way the game splits it
    for (uint32_t pos = 0; pos < n; pos += G_CHUNK) g_dec(ctx, (uint64_t)(p + pos), n - pos < G_CHUNK ? n - pos : G_CHUNK);
}
// 搬入：输出 [起点, 起点 + olen) 与 ctx 会写字节；四个指针按输出缓冲地址重定位
// Move in: output [start, start + olen) and the bytes of ctx that get written; the four pointers are relocated to the output buffer address
static void g_materialize(uint8_t *ctx, gseg *s, uint64_t dst) {
    memcpy((void *)dst, s->out, s->olen);
    for (uint32_t i = 0x20; i < G_CTX; i++) if (s->wm[i >> 3] >> (i & 7) & 1) ctx[i] = s->ctx[i];
    for (int k = 0; k < 4; k++) { uint64_t p; memcpy(&p, s->ctx + 8 * k, 8); p = p - (uint64_t)s->out + dst; memcpy(ctx + 8 * k, &p, 8); }
}
static int g_compare(const uint8_t *ctx, gseg *s, uint64_t dst) {   // 校验模式：真解码终态 vs 预解结果 | verify mode: real decode final state vs pre-decoded result
    int bad = 0;
    if (memcmp((void *)dst, s->out, s->olen)) { bad = 1; atomic_fetch_add(&g_vwhy[0], 1); }
    for (uint32_t i = 0x20; i < G_CTX; i++) if ((s->wm[i >> 3] >> (i & 7) & 1) && ctx[i] != s->ctx[i]) {
        bad = 1; atomic_fetch_add(&g_vwhy[1], 1); atomic_fetch_add(&g_voff[i >> 10], 1);
        static _Atomic int shown; if (atomic_fetch_add(&shown, 1) < 4) LOG("DM_GRANNY_VERIFY 例：ctx +%#x 真 %02x 预 %02x（clen %u dlen %u）", i, ctx[i], s->ctx[i], s->clen, s->dlen);
        break; }
    for (int k = 0; k < 4; k++) { uint64_t a, b; memcpy(&a, ctx + 8 * k, 8); memcpy(&b, s->ctx + 8 * k, 8); if (a - dst != b - (uint64_t)s->out) { bad = 1; atomic_fetch_add(&g_vwhy[2], 1); break; } }
    return !bad;
}
static uint64_t g_chunk(uint64_t *regs, uint32_t clen, uint32_t pos) {
    uint64_t ctx = regs[0]; const uint8_t *buf = (const uint8_t *)regs[1]; uint32_t len = (uint32_t)regs[2];
    if (g_gmode < 0) granny_init();
    if (g_gmode > 0) {
        if (pos == 0) {
            if (t_g.c) { g_release(t_g.c); t_g.c = NULL; }             // 上一段没走到末块（读失败提前出循环）：作废 | the previous segment did not reach the last chunk (read failure left the loop early): invalidate
            uint8_t *c = (uint8_t *)ctx; uint64_t c58, c80, c10, c18; memcpy(&c58, c + 0x58, 8); memcpy(&c80, c + 0x80, 8); memcpy(&c10, c + 0x10, 8); memcpy(&c18, c + 0x18, 8);
            gseg *s = (!c58 && !c80) ? g_lookup(clen, buf, len) : NULL;   // 只认新建的 ctx | only accept a newly created ctx
            if (s && c18 - c10 != s->dlen) { g_release(s); s = NULL; }
            if (s) { t_g.ctx = ctx; t_g.dst = c10; t_g.c = s; t_g.ver = 0; } else atomic_fetch_add(&g_miss, 1);
        }
        gseg *s = t_g.c;
        if (s && t_g.ctx == ctx && t_g.ver == pos && pos + len <= s->clen && !memcmp(s->cmp + pos, buf, len)) {
            if (g_gmode == 2) {                                         // 校验：照常真解，末块后比 | verify: really decode as usual, compare after the last chunk
                uint64_t r = g_dec(ctx, (uint64_t)buf, len); t_g.ver += len;
                if (t_g.ver == s->clen) { atomic_fetch_add(g_compare((uint8_t *)ctx, s, t_g.dst) ? &g_vok : &g_vbad, 1); g_release(s); t_g.c = NULL; }
                return r;
            }
            t_g.ver += len;
            if (t_g.ver == s->clen) { g_materialize((uint8_t *)ctx, s, t_g.dst); g_release(s); t_g.c = NULL; atomic_fetch_add(&g_hit, 1); }
            return 1;
        }
        if (s && t_g.ctx == ctx) {                                      // 对不上：补喂已推迟的块，转当场解 | mismatch: feed the deferred chunks to the decoder, switch to decoding on the spot
            if (g_gmode == 1) g_feed(ctx, s->cmp, t_g.ver);
            g_release(s); t_g.c = NULL; atomic_fetch_add(&g_fallback, 1);
        }
    }
    return g_dec(ctx, (uint64_t)buf, len);
}
static uint64_t granny_chunk(uint64_t *regs, uint32_t clen, uint32_t pos, int path) {
    if (!g_dec) g_fns();
    if (g_gprof < 0) g_gprof = getenv("DM_GRANNY_PROF") != NULL;
    if (!g_gprof) return g_chunk(regs, clen, pos);
    if (pos == 0) { uint64_t a, b; memcpy(&a, (uint8_t *)regs[0] + 0x10, 8); memcpy(&b, (uint8_t *)regs[0] + 0x18, 8);
                    uint64_t rib = dm_prof_ribbon_ns(); int ph = rib && now_ns() >= rib;
                    atomic_fetch_add(&g_gpseg[ph][path], 1); atomic_fetch_add(&g_gpdl[ph][path], b - a); }
    uint64_t t0 = now_ns(), r = g_chunk(regs, clen, pos); gp_add(t0, (uint32_t)regs[2]); return r;
}
void svc_granny_chunk_a(uint64_t *regs) { regs[0] = granny_chunk(regs, (uint32_t)regs[24], (uint32_t)regs[28], 0) & 0xffffffff; }
void svc_granny_chunk_b(uint64_t *regs) { regs[0] = granny_chunk(regs, (uint32_t)regs[25], (uint32_t)regs[26], 1) & 0xffffffff; }
