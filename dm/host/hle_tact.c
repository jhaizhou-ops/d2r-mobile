// TACT 存储（暴雪 CASC 库）BLTE 块的 zlib 解码 → libdeflate（第三方，dm/third_party/libdeflate，MIT）。
// 接管点：main 0x1b0ee60（库内部函数，无符号，直接 bl 调用）。剪口把入口两条指令换成 `svc #0x80; ret`，
// 改写器把 svc 变成宿主门，这里按 0x80 分派（Horizon SVC 号 ≤0x7F，0x80 起约定为宿主调用）。
// 原函数：status DecodeZlib(u8 *out, u32 out_cap, const u8 *in, u32 in_len, u32 flags, u32 stream, Obj *override)
//   override 非空：先调 override->vt[2](override, out, out_cap, in, in_len, 1)，返回假即返回 0；
//   flags bit0：输入首字节须是 'Z'（in_len ≥ 2），否则返回 8；跳过该字节；
//   之后 inflateInit + inflate(Z_FINISH)：到达流尾即返回 0（输出可短于 out_cap）；
//   inflate 失败按 0x1b0f12c 的字节表：Z_BUF_ERROR→4、Z_MEM/DATA/STREAM_ERROR→1、Z_ERRNO→3，其余→1。
//   stream 位（4 KiB 分段解压再拷贝）语义同为「整段流解进 out」，选角前实测 0 次，这里同样整段解。
// 等价依据：libdeflate_zlib_decompress 同样校验 zlib 头与 adler32、允许输出短于容量；
// 离线对拍选角前全部 4,979 个文件（558.8 MiB 输出）MD5 == 清单 CKey。
// zlib decode of BLTE blocks in TACT storage (Blizzard's CASC library) → libdeflate (third party, dm/third_party/libdeflate, MIT).
// Take-over point: main 0x1b0ee60 (a library-internal function, no symbol, called directly by bl). The cut replaces the two entry instructions with `svc #0x80; ret`,
// the rewriter turns the svc into a host gate, and here it is dispatched on 0x80 (Horizon SVC numbers are ≤0x7F; 0x80 and up are by convention host calls).
// Original function: status DecodeZlib(u8 *out, u32 out_cap, const u8 *in, u32 in_len, u32 flags, u32 stream, Obj *override)
//   override non-null: first call override->vt[2](override, out, out_cap, in, in_len, 1), and return 0 if it returns false;
//   flags bit0: the first input byte must be 'Z' (in_len ≥ 2), otherwise return 8; that byte is skipped;
//   then inflateInit + inflate(Z_FINISH): return 0 once the end of stream is reached (output may be shorter than out_cap);
//   inflate failures follow the byte table at 0x1b0f12c: Z_BUF_ERROR→4, Z_MEM/DATA/STREAM_ERROR→1, Z_ERRNO→3, everything else→1.
//   The stream bit (decompress in 4 KiB pieces, then copy) has the same semantics, "decode the whole stream into out"; measured 0 times before character select, and here it is likewise decoded in one piece.
// Basis for equivalence: libdeflate_zlib_decompress likewise verifies the zlib header and adler32 and allows output shorter than capacity;
// an offline cross-check of all 4,979 files before character select (558.8 MiB of output) gives MD5 == the list's CKey.
#include "hle.h"
#include "../third_party/libdeflate/libdeflate.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread/qos.h>
#include <stdatomic.h>

// DM_TACTCHK=<文件>：每次解码记一行「输入开头16字节(hex)\t输入长度\t输出长度\t输出MD5\t发起线程句柄」，离线与独立解码的逐块 MD5 对照（验收用）。
// 到选角（第一次 OpenPanel MainMenuButtonRibbon）即停记（启动块清单按此记录生成，边界须由事件定、不随杀进程时刻变）
// DM_TACTCHK=<file>: log one line per decode, "first 16 input bytes (hex)\tinput length\toutput length\toutput MD5\trequesting thread handle", to be compared offline against per-block MD5s of an independent decode (for acceptance).
// Logging stops on reaching character select (the first OpenPanel MainMenuButtonRibbon) (the boot block list is generated from this log, so the boundary must be set by an event, not by the moment the process is killed)
static FILE *g_chk; static pthread_mutex_t g_chk_lock = PTHREAD_MUTEX_INITIALIZER;
static void chk_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)cat; if (strcmp(arg, "MainMenuButtonRibbon") || !strstr(act, "OpenPanel")) return;
    pthread_mutex_lock(&g_chk_lock); if (g_chk) { fclose(g_chk); g_chk = NULL; LOG("DM_TACTCHK：到选角，停记"); } pthread_mutex_unlock(&g_chk_lock);
}
void tact_chk_init(void) {                            // 宿主初始化时调（main.c）：早于任何解码，免得首次解码时再懒开文件 | Called at host init (main.c): earlier than any decode, so the file is not lazily opened on the first decode
    const char *e = getenv("DM_TACTCHK"); if (!e) return;
    if (!(g_chk = fopen(e, "w"))) die("DM_TACTCHK 打不开 %s", e);
    void uimsg_subscribe(void (*)(const char *, const char *, const char *)); uimsg_subscribe(chk_on_uimsg);
}
static void chk_log(const uint8_t *in, uint32_t n, const uint8_t *out, size_t got) {
    if (!g_chk) return;                               // 未开或已停（无锁快判；下面持锁再判） | Not enabled or already stopped (lock-free quick check; checked again below under the lock)
    uint8_t md[16]; CC_MD5(out, (CC_LONG)got, md);
    pthread_mutex_lock(&g_chk_lock);
    if (!g_chk) { pthread_mutex_unlock(&g_chk_lock); return; }
    for (uint32_t i = 0; i < 16 && i < n; i++) fprintf(g_chk, "%02x", in[i]);
    uint32_t thr_cur_handle(void);
    fprintf(g_chk, "\t%u\t%zu\t", n, got); for (int i = 0; i < 16; i++) fprintf(g_chk, "%02x", md[i]); fprintf(g_chk, "\t%#x\n", thr_cur_handle());
    pthread_mutex_unlock(&g_chk_lock);
}

static __thread struct libdeflate_decompressor *t_dec;   // 每线程一个（解压器不可跨线程并用） | One per thread (a decompressor must not be shared across threads)
// 随线程退出释放（键析构，同 crash.c 备用栈）：原先不放，做过解码又退出的线程每条漏一个解压器（约 11.5 KB）
// Freed when the thread exits (key destructor, same as the alternate stack in crash.c): it used not to be freed, so every thread that decoded something and then exited leaked one decompressor (about 11.5 KB)
static pthread_key_t g_dec_key; static pthread_once_t g_dec_once = PTHREAD_ONCE_INIT;
static void dec_free(void *p) { libdeflate_free_decompressor(p); }
static void dec_key_init(void) { if (pthread_key_create(&g_dec_key, dec_free)) die("libdeflate 解压器键创建失败"); }
static struct libdeflate_decompressor *thread_dec(void) {
    if (t_dec) return t_dec;
    if (!(t_dec = libdeflate_alloc_decompressor())) die("libdeflate 分配解压器失败");
    pthread_once(&g_dec_once, dec_key_init); pthread_setspecific(g_dec_key, t_dec); return t_dec;
}

// ---------- DM_TACT_PREFETCH=<启动块清单>：后台预解码（性能提示，正确性不依赖它）----------
// 清单（dm/tools/tact_bootlist.py 由一局 DM_TACTCHK 生成）：选角前会请求的块，每行「数据文件 偏移 压缩长 解码长 开头16字节 发起线程」，
// 按发起线程分流、流内按首次请求顺序（各线程自身的请求次序稳定，线程间交错每局不同：实测另一局 2,840 次请求晚于全局最大下标、滞后至 1,649 块）。
// 低 QoS（UTILITY）后台线程（缺省 2，DM_TACT_PREFETCH_THREADS）每次给「提前量最少」的流解下一块：pread 压缩数据 + libdeflate 解进缓存；
// 未取走字节封顶（缺省 256 MiB，DM_TACT_PREFETCH_MB；缓存空时总放行一块 ⇒ 峰值 ≤ 封顶 + 最大单块）。
// 游戏调 0x80 时按（开头16字节, 长度）查：缓存块压缩数据与游戏输入逐字节相同才拷出（同一 libdeflate、同一输入 ⇒ 输出逐字节相同），
// 正在解的等它解完；查不到 / 不相同 / 还没轮到 → 照常当场解码。取走即释放。
// 工作线程 0x1005 消费异步加载任务队列，流内顺序每局也会乱（实测滞后中位 340、最大 1,609 块）⇒ 不按滞后丢弃（DM_TACT_PREFETCH_LAG 可设，缺省不丢），
// 内存只由封顶约束；到选角（MainMenuButtonRibbon）即停后台线程并释放剩余缓存（清单里本局没要的块不长期占内存）。
// ---------- DM_TACT_PREFETCH=<boot block list>: background pre-decode (a performance hint; correctness does not depend on it) ----------
// The list (generated by dm/tools/tact_bootlist.py from one DM_TACTCHK run): the blocks requested before character select, one per line as "data file, offset, compressed length, decoded length, first 16 bytes, requesting thread",
// split into streams by requesting thread, and within a stream ordered by first request (each thread's own request order is stable, the interleaving between threads differs every run: measured in another run, 2,840 requests came later than the global maximum index, lagging by up to 1,649 blocks).
// Low-QoS (UTILITY) background threads (default 2, DM_TACT_PREFETCH_THREADS) each time decode the next block of the stream with "the least lead": pread the compressed data + libdeflate decode into the cache;
// unconsumed bytes are capped (default 256 MiB, DM_TACT_PREFETCH_MB; when the cache is empty one block is always let through ⇒ peak ≤ cap + largest single block).
// When the game calls 0x80, look up by (first 16 bytes, length): the block is copied out only if the cached block's compressed data is byte-identical to the game's input (same libdeflate, same input ⇒ byte-identical output),
// a block being decoded is waited for; not found / not identical / not reached yet → decode on the spot as usual. A block is freed as soon as it is taken.
// Worker thread 0x1005 consumes the async load task queue, so the order within a stream is also shuffled every run (measured lag: median 340, max 1,609 blocks) ⇒ nothing is discarded by lag (DM_TACT_PREFETCH_LAG can set it, default no discard),
// memory is bounded by the cap alone; on reaching character select (MainMenuButtonRibbon) the background threads stop and the remaining cache is freed (blocks in the list that this run did not ask for do not hold memory long term).
enum { PF_EMPTY, PF_BUSY, PF_READY, PF_GONE };
typedef struct { uint16_t fi; uint8_t sid; uint64_t off; uint32_t n, dl, next, pos; uint8_t key[16]; _Atomic int st; uint8_t *cmp, *dec; } pf_ent;
#define PF_MAXS 8
static struct { uint32_t h, n, cap, *ix, next, cur, ev; } g_ps[PF_MAXS]; static int g_nps;   // 流：线程句柄、块数、下标表、后台下一个、已取走位置、丢弃扫描位置（均持 g_pfl） | Streams: thread handle, block count, index table, next one for the background, consumed position, discard-scan position (all under g_pfl)
static pf_ent *g_pf; static uint32_t g_npf; static int *g_pffd; static int g_npffd;
#define PF_HBITS 14
static uint32_t g_pfh[1u << PF_HBITS];                 // 键哈希 → 首个清单下标 + 1（同键按 next 链） | Key hash → first list index + 1 (same keys are chained through next)
static _Atomic int64_t g_pfout, g_pfpeak; static int64_t g_pfcap; static uint32_t g_pflag; static _Atomic int g_pfstop;
static _Atomic uint32_t g_pfhit, g_pfwait, g_pfmiss, g_pfdrop, g_pfdone; static _Atomic uint64_t g_pfwait_ns;
static pthread_mutex_t g_pfl = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pfc = PTHREAD_COND_INITIALIZER;
static unsigned pf_hash(const uint8_t *k, uint32_t n) { uint64_t h = n * 0x9E3779B97F4A7C15ull; for (int i = 0; i < 16; i++) h = (h ^ k[i]) * 0x100000001b3ull; return (unsigned)(h >> (64 - PF_HBITS)); }
static void pf_free(pf_ent *e) {                       // 持 g_pfl | Holding g_pfl
    if (e->dec) { atomic_fetch_sub(&g_pfout, (int64_t)e->dl + e->n); free(e->dec); free(e->cmp); e->dec = e->cmp = NULL; }
    atomic_store(&e->st, PF_GONE); pthread_cond_broadcast(&g_pfc);
}
static void pf_consumed(pf_ent *e) {                   // 持 g_pfl：推进所在流的已取走位置，丢弃本流落后 64 块以上的无人要块 | Holding g_pfl: advance the consumed position of the owning stream, and discard unwanted blocks of this stream that lag by 64 blocks or more
    typeof(g_ps[0]) *ps = &g_ps[e->sid];
    if (e->pos + 1 > ps->cur) ps->cur = e->pos + 1;
    for (; ps->ev + g_pflag < ps->cur; ps->ev++) { pf_ent *x = &g_pf[ps->ix[ps->ev]]; if (atomic_load(&x->st) == PF_READY) { pf_free(x); atomic_fetch_add(&g_pfdrop, 1); } }
}
static void *pf_worker(void *arg) {
    (void)arg; pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    struct libdeflate_decompressor *d = libdeflate_alloc_decompressor(); if (!d) die("libdeflate 分配解压器失败（预取）");
    for (;;) {
        pthread_mutex_lock(&g_pfl);
        int best = -1; int64_t lead = INT64_MAX;
        for (int k = 0; k < g_nps; k++) if (g_ps[k].next < g_ps[k].n && (int64_t)g_ps[k].next - g_ps[k].cur < lead) { lead = (int64_t)g_ps[k].next - g_ps[k].cur; best = k; }
        if (best < 0 || atomic_load(&g_pfstop)) { pthread_mutex_unlock(&g_pfl); break; }
        pf_ent *e = &g_pf[g_ps[best].ix[g_ps[best].next++]];
        while (!atomic_load(&g_pfstop) && atomic_load(&e->st) == PF_EMPTY && atomic_load(&g_pfout) > 0 && atomic_load(&g_pfout) + e->dl + e->n > g_pfcap) pthread_cond_wait(&g_pfc, &g_pfl);
        if (atomic_load(&g_pfstop)) { pthread_mutex_unlock(&g_pfl); break; }
        int ex = PF_EMPTY; int mine = atomic_compare_exchange_strong(&e->st, &ex, PF_BUSY);
        pthread_mutex_unlock(&g_pfl);
        if (!mine) continue;                           // 已被游戏当场解掉 / 丢弃 | Already decoded on the spot by the game / discarded
        uint8_t *c = malloc(e->n), *o = malloc(e->dl); size_t got = 0;
        int ok = c && o && dm_data_pread(g_pffd[e->fi], c, e->n, (off_t)e->off) == (ssize_t)e->n
                 && libdeflate_zlib_decompress(d, c, e->n, o, e->dl, &got) == LIBDEFLATE_SUCCESS && got == e->dl;
        if (ok) { void granny_offer(const uint8_t *, size_t); granny_offer(o, e->dl); }   // .model（gr2）⇒ 段入 Granny 预解码（hle_granny.c 拷走压缩段；须在发布 READY 之前，发布后游戏可能取走并释放） | .model (gr2) ⇒ offer the sections to Granny pre-decode (hle_granny.c copies the compressed sections away; must happen before publishing READY, after which the game may take and free the block)
        pthread_mutex_lock(&g_pfl);
        if (ok && atomic_load(&g_pfstop)) ok = 0;    // 已到选角：不再入缓存，交给下面的失败分支统一释放 | Character select already reached: no longer put into the cache, let the failure branch below do the single free
                                                     // （修：原先这里先 free 一次、置 ok = 0 后下面的 else 又 free 一次 ⇒ 重复释放；
                                                     //  只在「后台解完一块时恰已到选角」才触发，负载重、预取落后时多见——并发 accept 三局 SIGABRT）
                                                     // (Fix: this used to free once here and set ok = 0, then the else below freed again ⇒ double free;
                                                     //  it only triggers when "the background finishes decoding a block just as character select has been reached", mostly under heavy load with prefetch lagging -- three concurrent accept runs hit SIGABRT)
        if (ok) { e->cmp = c; e->dec = o; int64_t v = atomic_fetch_add(&g_pfout, (int64_t)e->dl + e->n) + e->dl + e->n;
                  for (int64_t pk = atomic_load(&g_pfpeak); v > pk && !atomic_compare_exchange_weak(&g_pfpeak, &pk, v);) {}
                  atomic_store(&e->st, PF_READY); atomic_fetch_add(&g_pfdone, 1); }
        else { free(c); free(o); atomic_store(&e->st, PF_GONE); }
        pthread_cond_broadcast(&g_pfc); pthread_mutex_unlock(&g_pfl);
    }
    libdeflate_free_decompressor(d); return NULL;
}
// 游戏解码前查缓存：命中返回 1（out 已填、*got 置好）
// Check the cache before the game decodes: returns 1 on a hit (out filled, *got set)
static int pf_take(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, size_t *got) {
    if (!g_pf || n < 16) return 0;
    for (uint32_t i1 = g_pfh[pf_hash(in, n)]; i1; i1 = g_pf[i1 - 1].next) {
        pf_ent *e = &g_pf[i1 - 1];
        if (e->n != n || memcmp(e->key, in, 16)) continue;
        int st = atomic_load(&e->st);
        if (st == PF_GONE) continue;
        if (st == PF_EMPTY) {                          // 还没轮到：同键唯一时认领（免得后台再白解），当场解 | Not reached yet: claim it if the key is unique (so the background does not decode it again for nothing) and decode on the spot
            pf_ent *nx = e->next ? &g_pf[e->next - 1] : NULL;
            if (!nx || nx->n != n || memcmp(nx->key, in, 16)) {
                int ex = PF_EMPTY;
                if (atomic_compare_exchange_strong(&e->st, &ex, PF_GONE)) { pthread_mutex_lock(&g_pfl); pf_consumed(e); pthread_mutex_unlock(&g_pfl); }
            }
            atomic_fetch_add(&g_pfmiss, 1); return 0;
        }
        pthread_mutex_lock(&g_pfl);
        if (atomic_load(&e->st) == PF_BUSY) {
            uint64_t t0 = now_ns(); atomic_fetch_add(&g_pfwait, 1);
            while (atomic_load(&e->st) == PF_BUSY) pthread_cond_wait(&g_pfc, &g_pfl);
            atomic_fetch_add(&g_pfwait_ns, now_ns() - t0);
        }
        if (atomic_load(&e->st) == PF_READY && !memcmp(e->cmp, in, n) && e->dl <= cap) {
            memcpy(out, e->dec, e->dl); *got = e->dl;
            pf_free(e); pf_consumed(e);
            pthread_mutex_unlock(&g_pfl); atomic_fetch_add(&g_pfhit, 1); return 1;
        }
        pthread_mutex_unlock(&g_pfl);                  // 同键不同内容：看链上下一个 | Same key, different content: look at the next one on the chain
    }
    atomic_fetch_add(&g_pfmiss, 1); return 0;
}
void tact_prefetch_stop(void) {                       // 优雅退出用：停后台预取线程（缓存随进程退出回收） | For graceful exit: stop the background prefetch threads (the cache is reclaimed when the process exits)
    if (!g_pf) return;
    pthread_mutex_lock(&g_pfl); atomic_store(&g_pfstop, 1); pthread_cond_broadcast(&g_pfc); pthread_mutex_unlock(&g_pfl);
}
static void tact_prefetch_finish(void);
static void pf_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)cat; if (!strcmp(arg, "MainMenuButtonRibbon") && strstr(act, "OpenPanel")) tact_prefetch_finish();
}
// 缺省开：不设 DM_TACT_PREFETCH 时用产物目录的 bootlist.tsv（没有就不预取）；DM_TACT_PREFETCH=0 关；=路径 指定清单。
// 清单头「# BUILD_ID」与产物 main.cuts.tsv 的构建号不符 ⇒ 整段不启用（正确性不依赖清单，只是不让过期清单浪费内存与带宽）；读不到产物构建号（如 iOS 内嵌模块）则不核。
// On by default: without DM_TACT_PREFETCH, bootlist.tsv in the build output directory is used (no prefetch if absent); DM_TACT_PREFETCH=0 off; =path specifies the manifest.
// If the manifest header "# BUILD_ID" does not match the build ID in the build output's main.cuts.tsv ⇒ the whole feature is not enabled (correctness does not depend on the manifest, this just keeps a stale manifest from wasting memory and bandwidth); if the build output's build ID cannot be read (e.g. iOS embedded modules) it is not checked.
static void build_id_of(const char *path, char *out, size_t n) {
    out[0] = 0; FILE *f = fopen(path, "r"); if (!f) return;
    char l[256]; if (fgets(l, sizeof l, f) && !strncmp(l, "# BUILD_ID", 10)) { char *p = l + 10; while (*p == '\t' || *p == ' ') p++; snprintf(out, n, "%.32s", p); }
    fclose(f);
}
static void tact_data_path(const char *rom, const char *name, char *out, size_t n) {   // 数据档：覆盖层各层里有就用覆盖层的，否则用资源目录的 | Data archive: use the overlay's copy if any overlay layer has it, otherwise the one in the resource directory
    char rel[300]; snprintf(rel, sizeof rel, "data/%s", name);
    if (!dm_overlay_file(rel, out, n)) snprintf(out, n, "%s/%s", rom, name);
}
void tact_prefetch_start(const char *outdir) {
    const char *e = getenv("DM_TACT_PREFETCH"); char def[1100];
    if (e && !strcmp(e, "0")) { LOG("DM_TACT_PREFETCH=0：启动块预取关闭"); return; }
    if (!e) { snprintf(def, sizeof def, "%s/bootlist.tsv", outdir); if (access(def, R_OK)) return; e = def; }
    char want[40], have[40], cuts[1100]; snprintf(cuts, sizeof cuts, "%s/main.cuts.tsv", outdir);
    build_id_of(cuts, want, sizeof want); build_id_of(e, have, sizeof have);
    if (want[0] && strcmp(want, have)) { LOG("DM_TACT_PREFETCH：清单 %s 的 main 构建号「%s」与产物「%s」不符，不启用预取", e, have[0] ? have : "无", want); return; }
    FILE *f = fopen(e, "r"); if (!f) die("DM_TACT_PREFETCH 打不开 %s", e);
    char rom[1024]; const char *rs = getenv("DM_ROMFS");
    if (rs) snprintf(rom, sizeof rom, "%s/data", rs); else snprintf(rom, sizeof rom, "%s%s/data/switch/3.3.94012/Update/RomFS/data", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work");
    char names[64][32]; uint32_t cap = 0; char line[256];
    while (fgets(line, sizeof line, f)) {
        char fn[32], kh[40]; unsigned long long off; unsigned n, dl, th = 0;
        int nf = sscanf(line, "%31s %llu %u %u %39s %x", fn, &off, &n, &dl, kh, &th);
        if (nf < 5 || strlen(kh) != 32) continue;
        int fi = 0; while (fi < g_npffd && strcmp(names[fi], fn)) fi++;
        if (fi == g_npffd) { if (g_npffd == 64) die("DM_TACT_PREFETCH：数据文件超过 64 个"); strcpy(names[g_npffd++], fn); }
        int si = 0; while (si < g_nps && g_ps[si].h != th) si++;
        if (si == g_nps) { if (g_nps == PF_MAXS) die("DM_TACT_PREFETCH：发起线程超过 %d 个", PF_MAXS); g_ps[g_nps++].h = th; }
        if (g_npf == cap) { cap = cap ? cap * 2 : 8192; g_pf = realloc(g_pf, cap * sizeof *g_pf); }
        pf_ent *x = &g_pf[g_npf]; memset(x, 0, sizeof *x); x->fi = (uint16_t)fi; x->sid = (uint8_t)si; x->off = off; x->n = n; x->dl = dl;
        for (int i = 0; i < 16; i++) { unsigned v; sscanf(kh + 2 * i, "%2x", &v); x->key[i] = (uint8_t)v; }
        typeof(g_ps[0]) *ps = &g_ps[si];
        if (ps->n == ps->cap) { ps->cap = ps->cap ? ps->cap * 2 : 1024; ps->ix = realloc(ps->ix, ps->cap * sizeof *ps->ix); }
        x->pos = ps->n; ps->ix[ps->n++] = g_npf;
        g_npf++;
    }
    fclose(f);
    if (!g_npf) die("DM_TACT_PREFETCH 清单为空：%s", e);
    g_pffd = malloc(g_npffd * sizeof *g_pffd);
    for (int i = 0; i < g_npffd; i++) { char p[1100]; tact_data_path(rom, names[i], p, sizeof p); if ((g_pffd[i] = open(p, O_RDONLY)) < 0) die("DM_TACT_PREFETCH 打不开数据文件 %s", p); dm_data_note_fd(g_pffd[i], p); }
    for (uint32_t i = g_npf; i-- > 0;) { unsigned h = pf_hash(g_pf[i].key, g_pf[i].n); g_pf[i].next = g_pfh[h]; g_pfh[h] = i + 1; }
    const char *mb = getenv("DM_TACT_PREFETCH_MB"), *nt = getenv("DM_TACT_PREFETCH_THREADS");
    g_pfcap = (mb ? atoll(mb) : 256) << 20; int th = nt ? atoi(nt) : 2;   // 缺省 256 MiB（真机启动期 footprint 峰值 ≤ 3 GB 的前提下；超了降 128） | default 256 MiB (on the premise that the real device boot-period footprint peak is ≤ 3 GB; lower to 128 if exceeded)
    const char *lg = getenv("DM_TACT_PREFETCH_LAG"); g_pflag = lg ? (uint32_t)atoi(lg) : UINT32_MAX / 2;
    for (int i = 0; i < th; i++) { pthread_t t; if (pthread_create(&t, NULL, pf_worker, NULL)) die("DM_TACT_PREFETCH 起线程失败"); pthread_detach(t); }
    char sb[256] = ""; for (int k = 0; k < g_nps; k++) snprintf(sb + strlen(sb), sizeof sb - strlen(sb), "%s%#x:%u", k ? " " : "", g_ps[k].h, g_ps[k].n);
    LOG("DM_TACT_PREFETCH：清单 %u 块（%d 个数据文件；按线程分流 %s），%d 线程，封顶 %lld MiB", g_npf, g_npffd, sb, th, (long long)(g_pfcap >> 20));
    void uimsg_subscribe(void (*)(const char *, const char *, const char *)); uimsg_subscribe(pf_on_uimsg);   // 到选角收尾（不依赖 DM_BOOTPROF） | wrap-up on reaching character select (does not depend on DM_BOOTPROF)
}
static void tact_prefetch_finish(void) {             // 到选角：停后台、释放剩余缓存、报账（只做一次） | on reaching character select: stop the background, release the remaining cache, report the totals (done only once)
    static _Atomic int once; if (!g_pf || atomic_exchange(&once, 1)) return;
    pthread_mutex_lock(&g_pfl);
    atomic_store(&g_pfstop, 1); uint32_t left = 0;
    for (uint32_t i = 0; i < g_npf; i++) if (atomic_load(&g_pf[i].st) == PF_READY) { pf_free(&g_pf[i]); left++; }
    pthread_cond_broadcast(&g_pfc); pthread_mutex_unlock(&g_pfl);
    { void granny_finish(const char *); granny_finish("到选角"); }
    if (left) LOG("DM_TACT_PREFETCH：到选角时缓存里还有 %u 块没人要，已释放", left);
    LOG("DM_TACT_PREFETCH：命中 %u（其中等后台解完 %u，共等 %.3f s）未命中 %u 丢弃 %u；缓存峰值 %.1f MiB；后台已解 %u / %u 块",
        atomic_load(&g_pfhit), atomic_load(&g_pfwait), atomic_load(&g_pfwait_ns) / 1e9, atomic_load(&g_pfmiss), atomic_load(&g_pfdrop),
        atomic_load(&g_pfpeak) / 1048576.0, atomic_load(&g_pfdone), g_npf);
}

static uint32_t tact_decode_zlib(uint64_t *r) {
    uint8_t *out = (uint8_t *)r[0]; uint32_t cap = (uint32_t)r[1];
    const uint8_t *in = (const uint8_t *)r[2]; uint32_t n = (uint32_t)r[3], flags = (uint32_t)r[4];
    void **ov = (void **)r[6];
    if (ov) {
        uint32_t (*cb)(void *, void *, uint32_t, const void *, uint32_t, uint32_t) = (*(void ***)ov)[2];
        if (!(cb(ov, out, cap, in, n, 1) & 0xff)) return 0;
    }
    if (flags & 1) {
        if (n < 2 || in[0] != 'Z') { LOG("tact：zlib 块编码字节不是 'Z'（%#x）", n ? in[0] : 0); return 8; }
        in++; n--;
    }
    thread_dec();
    size_t got = 0; uint64_t t0 = now_ns();
    if (pf_take(in, n, out, cap, &got)) { dm_prof_add(PROF_TACT, now_ns() - t0, got); chk_log(in, n, out, got); return 0; }
    enum libdeflate_result rc = libdeflate_zlib_decompress(t_dec, in, n, out, cap, &got);
    dm_prof_add(PROF_TACT, now_ns() - t0, got);
    if (rc == LIBDEFLATE_SUCCESS) { chk_log(in, n, out, got); return 0; }
    LOG("tact：zlib 解码失败 %d（输入 %u，容量 %u）", rc, n, cap);
    return rc == LIBDEFLATE_INSUFFICIENT_SPACE ? 4 : 1;
}

// 私有Render门：弱实现走原图；UI强实现只读确认cohort。x8临时结果，跳板两路恢复x8。
// Private Render gate: the weak implementation takes the original picture; the UI strong implementation only reads to confirm the cohort. x8 is the temporary result, both trampoline paths restore x8.
__attribute__((weak)) int dm_touch_game_hide_own_glyph(uint64_t widget) { (void)widget; return 0; }
// hle_svc.c 的 svc_dispatch 对 0x80 起调这里；0x85保x0。
// svc_dispatch in hle_svc.c calls here for 0x80 and up; 0x85 preserves x0.

uint64_t svc_host_call(int num, uint64_t *regs) {
    switch (num) {
    case SVC_HOST_PRIVATE_SKILL_AIM: { void svc_private_skill_aim(uint64_t *); svc_private_skill_aim(regs); return 1; }
    case SVC_HOST_PRIVATE_SKILL_RENDER: {
        // 「不画原版技能格图标」只在触屏模式要（图标由我们的技能圈画）；手柄模式是原版底栏，图标得照画——先前不分模式，手柄版底栏六格全空（线上 iPad + 实体手柄反馈，Mac 上复现）
        // "Do not draw the original skill slot icons" is only wanted in touch mode (the icons are drawn by our skill ring); controller mode uses the
        // original bottom bar, so the icons must be drawn as usual -- earlier the modes were not told apart and all six slots of the controller bottom bar
        // were empty (reported from a released build on iPad + physical controller, reproduced on Mac)
        { int dm_padmode_current(void); regs[8] = !dm_padmode_current() && dm_touch_game_hide_own_glyph(regs[19]) ? 1 : 0; }
        const char *path=getenv("DM_SKILL_RENDER_GATE_LOG");
        if(path) { static FILE *fp; static unsigned n[2]; if(!fp)fp=fopen(path,"w");
            if(fp && n[regs[8]]++ < 64) { fprintf(fp,"widget=%#llx match=%llu\n",regs[19],regs[8]);fflush(fp); } }
        return 1;
    }
    case SVC_HOST_TACT_ZLIB: regs[0] = tact_decode_zlib(regs); return 1;
    case SVC_HOST_C2S_DELIVER: { void c2s_deliver(uint64_t *); c2s_deliver(regs); return 1; }
    case SVC_HOST_C2S_SYS: { void c2s_deliver_sys(uint64_t *); c2s_deliver_sys(regs); return 1; }
    case SVC_HOST_GRANNY_A: { void svc_granny_chunk_a(uint64_t *); svc_granny_chunk_a(regs); return 1; }
    case SVC_HOST_DROP_SOUND: { void svc_drop_sound(uint64_t *); svc_drop_sound(regs); return 1; }
    case SVC_HOST_GROUND_ASSET: { void svc_ground_asset(uint64_t *); svc_ground_asset(regs); return 1; }
    case SVC_HOST_GRANNY_B: { void svc_granny_chunk_b(uint64_t *); svc_granny_chunk_b(regs); return 1; }
    case SVC_HOST_RIFT_EXIT: { void svc_rift_exit(uint64_t *); svc_rift_exit(regs); return 1; }
    case SVC_HOST_JOIN_SPAWN: case SVC_HOST_JOIN_CORPSE: { void svc_join_spawn(uint64_t *, int); svc_join_spawn(regs, num == SVC_HOST_JOIN_CORPSE); return 1; }
    case SVC_HOST_SAVE_ACT: { void svc_save_act(uint64_t *); svc_save_act(regs); return 1; }
    case SVC_HOST_MERC_SLOT: { void svc_merc_slot(uint64_t *); svc_merc_slot(regs); return 1; }
    case SVC_HOST_MERC_SLOT_CL: { void svc_merc_slot_cl(uint64_t *); svc_merc_slot_cl(regs); return 1; }
    case SVC_HOST_CAM_AHEAD: { void svc_cam_ahead(uint64_t *); svc_cam_ahead(regs); return 1; }
    }
    return 0;
}
