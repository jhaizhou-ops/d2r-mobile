// 过场 / 背景视频剪口（movie:: = subsdk0 任天堂 movie 库；视频内容在复刻排除范围内，不解码、不出画面）。
// 语义：每段视频按容器里的真实时长「空播」，时序与原版一致——一次性视频到时长走游戏自己的「播完」路径（按键跳过照常生效），
// 循环视频（选角背景 logoloop）由游戏自己 SeekTo 回开头、按原时长空转。排除的是内容，状态机不剪。
// 游戏侧协议（main 3.3.94012 原映像偏移，平台线 10-01 读）：
//   播放器 P = [0x2e29b88]。Setup 0x8996d0：Ext 槽15 SetDataSource(读取器, 路径) → 槽4 GetTrackCount → Decoder 构造
//     → Dec 槽3 RegisterClientEvents(&P+0x1b8 = {输入事件, 输出事件, 种类3, 种类4}) → 槽2 Configure → 启动解码线程 → 槽5 Start；
//     Open 返回 (Start 状态 == 0)。
//   解码线程 0x899270：WaitAny；种类 1（输入可用）：Dec 槽9 AcquireInputBufferIndex → 槽10 GetInputBuffer → Ext 槽10 Read：
//     0x2000 = 流尾 ⇒ 循环（P+0x12）则 Ext 槽8 SeekTo(0)，否则置 P+0x488、标志 4；→ Ext 槽12 GetSampleTime
//     → Dec 槽11 SendInputBufferForDecode(idx, off, size, pts, 标志) → Ext 槽11 Advance。
//     种类 2（输出可用）：Dec 槽12 AcquireOutputBufferIndex(&idx,&pts,&标志,&剩余) 状态 0/0x2000 ⇒ 入帧队列。
//   每帧 0xbbfcf4 → 0x898230：帧队列空时返回 !P[+0x489]；取帧 0x89a4b0：Dec 槽18(idx, MediaData*) 非 0 且标志 4 且非循环
//     ⇒ P+0x489 = 1 ⇒ 游戏走正常播完 0xbbfb60。丢帧 0x899bc8 只在队列 ≥2 时（单个流尾项不会被丢）。
// 本剪口的时间线：Start 记 t0、挂定时 → 时长内不发任何事件（0 帧，游戏视为在播）→ 到时长由客体计时线程发「输入可用」
//   → Read 报 0x2000 →（循环）SeekTo 重挂定时 /（一次性）Send(标志 4) 时在解码线程上发「输出可用」→ Acquire 交出标志 4 项。
// 时钟：宿主单调时钟（不按帧）。未用到的虚槽一律报红陷阱（被调到即中止并报名），不靠桩返回 0 侥幸通过。
#include "hle_ohos_compat.h"
#include <time.h>

#define MV_SKIP_NS   100000000ll   // 不带视频数据时每段「播放」的时长：0.1 秒
#define ST_OK        0
#define ST_EOS       0x2000      // movie::Status 流尾（游戏 0x8995b8 比较的值）
#define ST_TRY_AGAIN 0x1008      // 暂无缓冲（游戏 0x899564 自己用的值）：AcquireOutputBufferIndex 无输出时回它，游戏不入队
#define FLAG_EOS     4
#define NN_OS_DEFAULT_THREAD_PRIORITY 16

static int64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ll + t.tv_nsec; }
#define MVLOG(fmt, ...) do { if (g_flag.movie) LOG("movie 空播 [%.3f] " fmt, (now_ns() - g_t0_ns) / 1e9, ##__VA_ARGS__); } while (0)

// ---------- WebM 时长：EBML Segment/Info 的 Duration × TimecodeScale ----------
// rd(ctx, 偏移, 缓冲, 长度) 返回读到字节数。读不到 / 没有 Duration 返回 -1（调用方报红，不给缺省值）。
typedef int64_t (*mv_read_fn)(void *ctx, int64_t off, void *buf, size_t n);
static int ebml_vint(const uint8_t *p, size_t n, int keep_marker, uint64_t *v) {   // 返回长度，0 = 非法
    if (!n || !p[0]) return 0;
    int len = __builtin_clz((unsigned)p[0]) - 23;          // 首字节前导 0 个数 + 1
    if (len < 1 || len > 8 || (size_t)len > n) return 0;
    uint64_t x = keep_marker ? p[0] : (p[0] & (0xFF >> len));
    for (int i = 1; i < len; i++) x = (x << 8) | p[i];
    *v = x; return len;
}
int64_t mv_webm_duration_ns(mv_read_fn rd, void *ctx) {
    uint8_t b[64]; uint64_t id, sz; int a, c;
    int64_t off = 0, seg_end = -1;
    for (int depth = 0; depth < 2;) {                       // depth0：顶层找 Segment；depth1：Segment 内找 Info
        int64_t got = rd(ctx, off, b, sizeof b);
        if (got < 2 || !(a = ebml_vint(b, got, 1, &id)) || !(c = ebml_vint(b + a, got - a, 0, &sz))) return -1;
        int unknown = sz == (1ull << (7 * c)) - 1;          // 全 1 = 未知长度
        int64_t body = off + a + c;
        if (depth == 0 && id == 0x18538067) { seg_end = unknown ? INT64_MAX : body + (int64_t)sz; off = body; depth = 1; continue; }
        if (depth == 1 && id == 0x1549A966) {               // Info
            if (unknown || sz > 4096) return -1;
            uint8_t info[4096]; if (rd(ctx, body, info, sz) != (int64_t)sz) return -1;
            uint64_t scale = 1000000; double dur = -1;      // TimecodeScale 缺省 1ms（Matroska 规范缺省值）
            for (size_t p = 0; p < sz;) {
                uint64_t eid, esz; int ea = ebml_vint(info + p, sz - p, 1, &eid); if (!ea) return -1;
                int ec = ebml_vint(info + p + ea, sz - p - ea, 0, &esz); if (!ec || p + ea + ec + esz > sz) return -1;
                const uint8_t *e = info + p + ea + ec;
                if (eid == 0x2AD7B1) { scale = 0; for (uint64_t i = 0; i < esz; i++) scale = (scale << 8) | e[i]; }
                else if (eid == 0x4489 && (esz == 4 || esz == 8)) {
                    uint64_t r = 0; for (uint64_t i = 0; i < esz; i++) r = (r << 8) | e[i];
                    if (esz == 4) { uint32_t r32 = (uint32_t)r; float f; memcpy(&f, &r32, 4); dur = f; } else memcpy(&dur, &r, 8);
                }
                p += ea + ec + esz;
            }
            return dur > 0 && scale ? (int64_t)(dur * (double)scale + 0.5) : -1;
        }
        if (depth == 1 && id == 0x1F43B675) return -1;      // 到 Cluster 还没见到 Info
        if (unknown) return -1;
        off = body + (int64_t)sz;
        if (depth == 1 && off >= seg_end) return -1;
    }
    return -1;
}

// ---------- 播放状态（同一时刻游戏只有一个播放器 P；按对象登记，交接靠「最近一次取源」）----------
typedef struct {
    int64_t dur_ns, deadline;         // deadline：下一次发「输入可用」的单调时刻（0 = 未挂）
    int64_t fired;                    // 上一次到点的截止时刻：循环按媒体时间续接（新截止 = 上次截止 + 时长），不累积唤醒延迟
    void *ev_in, *ev_out;             // 游戏的 nn::os::EventType*（DecoderEvents[0] / [1]）
    int eos_out;                      // 已收到标志 4 输入，待交出流尾输出项
    int64_t last_pts_us;
} mv_t;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static mv_t *g_pending;               // 最近一次 SetDataSource 解出的状态，等 Decoder 构造时认领
static mv_t *g_armed;                 // 计时线程盯着的那一个
static void (*g_signal_event)(void *);

// 客体计时线程：经 SDK nn::os::CreateThread 建（入口是本函数），这样 SignalEvent 是在正规客体线程上调的
static void timer_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        mv_t *m = g_armed;
        if (!m || !m->deadline) { pthread_cond_wait(&g_cv, &g_lock); continue; }
        int64_t now = mono_ns();
        if (now < m->deadline) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            int64_t t = ts.tv_sec * 1000000000ll + ts.tv_nsec + (m->deadline - now);
            ts.tv_sec = t / 1000000000; ts.tv_nsec = t % 1000000000;
            pthread_cond_timedwait(&g_cv, &g_lock, &ts); continue;
        }
        g_armed = NULL; m->fired = m->deadline;           // 一次定时只发一次；持锁发，和 Stop / 删除串行
        MVLOG("到时长，发输入可用");
        g_signal_event(m->ev_in);
    }
}
static void timer_ensure(void) {                          // 在客体线程上调（Decoder::Start 里）
    static int started; if (started) return; started = 1;
    static _Alignas(16) uint8_t thread_obj[0x400];
    uint32_t (*create)(void *, void (*)(void *), void *, void *, size_t, int) =
        guest_sym("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    void (*start)(void *) = guest_sym("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    g_signal_event = guest_sym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (!create || !start || !g_signal_event) die("movie 剪口：缺 nn::os 线程 / 事件符号");
    size_t ss = 0x10000; void *stack = aligned_alloc(0x1000, ss);
    uint32_t r = create(thread_obj, timer_main, NULL, stack, ss, NN_OS_DEFAULT_THREAD_PRIORITY);
    if (r) die("movie 剪口：建计时线程失败 %#x", r);
    start(thread_obj);
}
static void arm(mv_t *m, int64_t at) {                    // 持锁调用
    m->deadline = at; g_armed = m; pthread_cond_signal(&g_cv);
}

// SetAllocator 登记的客体分配器：库对象由游戏用同一分配器申请后原地构造，删除析构须用登记的 free 还回去
static void (*g_free)(void *, void *); static void *g_user;
HLE_FORCE("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", "过场空播：登记客体分配器（删除析构用其 free）",
          void, mv_SetAllocator, void *alloc, void (*fr)(void *, void *), void *realloc_, void *user) {
    (void)alloc; (void)realloc_; g_free = fr; g_user = user;
}

// ---- 虚表：对象首字 = 虚表地址点，次字 = 本剪口的 mv_t*（游戏为库对象分配的空间远大于 16 字节）----
#define TRAP(cls, slot, what) static void mv_trap_##cls##_##slot(void) { die("movie 剪口：%s 槽%d %s 被调用（不在空播协议内）", #cls, slot, what); }
static mv_t *M(void *self) { return ((mv_t **)self)[1]; }
static void mv_dtor(void *self) { (void)self; }
static void mv_delete(void *self) {
    pthread_mutex_lock(&g_lock);
    mv_t *m = M(self);
    if (m && g_armed == m) g_armed = NULL;
    if (m && g_pending == m) g_pending = NULL;
    pthread_mutex_unlock(&g_lock);
    if (!g_free) die("movie 剪口：未登记分配器");
    g_free(self, g_user);
}

// ---- 游戏的 ClientStreamReader（main 虚表 0x20b0e90）：槽2 bool Open(路径, void**h)、槽3 ReadAt(h,off,buf,n)、槽4 Close(h) ----
typedef struct { void **reader; void *h; } rdctx;
static int64_t rd_reader(void *c_, int64_t off, void *buf, size_t n) {
    rdctx *c = c_; void **vt = *(void ***)c->reader;
    return ((int64_t (*)(void *, void *, int64_t, void *, size_t))vt[3])(c->reader, c->h, off, buf, n);
}

// Extractor（16 槽）
static int ext_SetDataSourceReader(void *self, void **reader, const char *path) {
    // 过场 / 背景视频是产品排除项（jhz 09-23 / 09-29 / 10-01：过场动画不接，游戏自然跳过）：资源包里不带视频数据，这里不打开文件、不读时长，
    // 一律按 MV_SKIP_NS 即「播完」——一次性视频走游戏自己的播完路径，循环视频（选角背景 logoloop）按这个周期空转。
    // SW_MOVIE_REAL=1：按容器里的真实时长空播（10-03 之前的行为；对真值时序用，需要带视频数据的资源包）。
    (void)reader; int64_t d = MV_SKIP_NS;
    static int real = -1; if (real < 0) { const char *e = getenv("SW_MOVIE_REAL"); real = e && !strcmp(e, "1"); }
    if (real) {
        void **vt = *(void ***)reader; rdctx c = {reader, NULL};
        if (!(((uint32_t (*)(void *, const char *, void **))vt[2])(reader, path, &c.h) & 1)) die("movie 剪口：读取器打不开 %s", path);
        d = mv_webm_duration_ns(rd_reader, &c);
        ((void (*)(void *, void *))vt[4])(reader, c.h);
        if (d <= 0) die("movie 剪口：%s 读不出 WebM 时长（Segment/Info/Duration）", path);
    }
    mv_t *m = calloc(1, sizeof *m); m->dur_ns = d;
    pthread_mutex_lock(&g_lock); ((mv_t **)self)[1] = m; g_pending = m; pthread_mutex_unlock(&g_lock);
    MVLOG("%s 时长 %.3f 秒", path, d / 1e9);
    return ST_OK;
}
static int ext_GetTrackCount(void *self, int *n) { (void)self; *n = 0; return ST_OK; }   // 0 轨 ⇒ Setup 跳过选轨
static int ext_SeekTo(void *self, int64_t us, int mode) {   // 循环视频回开头：重新计一轮时长
    (void)us; (void)mode; mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); arm(m, (m->fired ? m->fired : mono_ns()) + m->dur_ns); m->last_pts_us = 0; pthread_mutex_unlock(&g_lock);
    MVLOG("循环回开头");
    return ST_OK;
}
static int ext_Read(void *self, void *buf) {
    (void)buf; mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); int64_t dl = m->deadline; pthread_mutex_unlock(&g_lock);
    if (dl && mono_ns() < dl) die("movie 剪口：未到时长就被要求读样本（协议外）");
    m->last_pts_us = m->dur_ns / 1000;
    return ST_EOS;
}
static int ext_Advance(void *self) { (void)self; return ST_OK; }
static int ext_GetSampleTime(void *self, int64_t *us) { *us = M(self)->last_pts_us; return ST_OK; }
TRAP(ext, 2, "SetDataSource(char const*)") TRAP(ext, 3, "SetDataSource(void const*, int)") TRAP(ext, 5, "GetTrackConfiguration")
TRAP(ext, 6, "SelectTrack") TRAP(ext, 7, "UnselectTrack") TRAP(ext, 9, "GetTrackIndexForAvailableData")
TRAP(ext, 13, "GetSampleMetaData") TRAP(ext, 14, "GetBufferedRange")
static void *const g_ext_vt[16] = {
    mv_dtor, mv_delete, mv_trap_ext_2, mv_trap_ext_3, ext_GetTrackCount, mv_trap_ext_5, mv_trap_ext_6, mv_trap_ext_7,
    ext_SeekTo, mv_trap_ext_9, ext_Read, ext_Advance, ext_GetSampleTime, mv_trap_ext_13, mv_trap_ext_14,
    ext_SetDataSourceReader,
};

// Decoder（19 槽）
static int dec_Configure(void *self, void *md) { (void)self; (void)md; return ST_OK; }
static int dec_RegisterClientEvents(void *self, void **ev) { mv_t *m = M(self); m->ev_in = ev[0]; m->ev_out = ev[1]; return ST_OK; }
static int dec_Start(void *self) {
    timer_ensure();
    MVLOG("Start");
    mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); arm(m, mono_ns() + m->dur_ns); pthread_mutex_unlock(&g_lock);
    return ST_OK;
}
static int dec_Stop(void *self) {                          // 按键跳过 / 收尾：立即撤销定时
    mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); m->deadline = 0; if (g_armed == m) g_armed = NULL; pthread_mutex_unlock(&g_lock);
    return ST_OK;
}
static int dec_AcquireInputBufferIndex(void *self, int *idx, int *remaining) {
    mv_t *m = M(self);
    pthread_mutex_lock(&g_lock); int due = m->deadline && mono_ns() >= m->deadline; pthread_mutex_unlock(&g_lock);
    *idx = due ? 0 : -1; *remaining = 0;
    MVLOG("取输入缓冲 %s", due ? "到时" : "未到");
    return ST_OK;
}
static uint8_t g_inbuf[16];
typedef struct { void *data; uint64_t cap; int32_t off, size; } mvbuf;   // movie::Buffer 的宿主布局（游戏栈上留了 ≥0x40 字节）
static int dec_GetInputBuffer(void *self, int idx, mvbuf *b) {
    (void)self; (void)idx; b->data = g_inbuf; b->cap = sizeof g_inbuf; b->off = b->size = 0; return ST_OK;
}
static int dec_SendInputBufferForDecode(void *self, int idx, int off, int size, int64_t pts, int flags) {
    (void)idx; (void)off; (void)size; (void)pts; mv_t *m = M(self);
    if (flags & FLAG_EOS) {                                 // 一次性视频播到头：在解码线程上直接发「输出可用」
        pthread_mutex_lock(&g_lock); m->eos_out = 1; m->deadline = 0; pthread_mutex_unlock(&g_lock);
        MVLOG("播完");
        g_signal_event(m->ev_out);
    }
    return ST_OK;
}
static int dec_AcquireOutputBufferIndex(void *self, int *idx, int64_t *pts, int *flags, int *remaining) {
    mv_t *m = M(self); *remaining = 0;
    pthread_mutex_lock(&g_lock); int eos = m->eos_out; m->eos_out = 0; pthread_mutex_unlock(&g_lock);
    if (!eos) return ST_TRY_AGAIN;                          // 无输出 ⇒ 游戏不入队
    *idx = 0; *pts = m->dur_ns / 1000; *flags = FLAG_EOS;
    return ST_OK;
}
static int dec_ReleaseOutputBufferIndex(void *self, int idx) { (void)self; (void)idx; return ST_OK; }
static int dec_GetOutputBufferFormat(void *self, int idx, void *md) { (void)self; (void)idx; (void)md; return ST_EOS; }   // 非 0 ⇒ 取帧按流尾项处理
TRAP(dec, 4, "GetOutputFormat") TRAP(dec, 7, "Reset") TRAP(dec, 8, "Flush")
TRAP(dec, 13, "GetOutputBuffer") TRAP(dec, 14, "GetOutputBufferSize")
TRAP(dec, 16, "GetLastError") TRAP(dec, 17, "GetOutputBuffer(MediaData)")
static void *const g_dec_vt[19] = {
    mv_dtor, mv_delete, dec_Configure, dec_RegisterClientEvents, mv_trap_dec_4, dec_Start, dec_Stop, mv_trap_dec_7,
    mv_trap_dec_8, dec_AcquireInputBufferIndex, dec_GetInputBuffer, dec_SendInputBufferForDecode, dec_AcquireOutputBufferIndex,
    mv_trap_dec_13, mv_trap_dec_14, dec_ReleaseOutputBufferIndex, mv_trap_dec_16, mv_trap_dec_17, dec_GetOutputBufferFormat,
};

HLE_FORCE("_ZN5movie9ExtractorC1ENS_13ContainerTypeE", "过场空播：Extractor 装空播虚表（取源时读 WebM 时长）",
          void, mv_Extractor, void **self, int type) { (void)type; self[0] = (void *)g_ext_vt; self[1] = NULL; }
HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "过场空播：Decoder 装空播虚表（按时长发流尾）",
          void, mv_Decoder, void **self, int type, int fmt, int mode, int prio) {
    (void)type; (void)fmt; (void)mode; (void)prio;
    pthread_mutex_lock(&g_lock); mv_t *m = g_pending; g_pending = NULL; pthread_mutex_unlock(&g_lock);
    if (!m) die("movie 剪口：Decoder 构造前没有取过源");
    self[0] = (void *)g_dec_vt; self[1] = m;
}
HLE_FORCE("_ZN5movie17GetThreadPriorityEv", "过场空播：只作 Decoder 构造参数", int, mv_GetThreadPriority, void) { return NN_OS_DEFAULT_THREAD_PRIORITY; }

// MediaData：Setup / 取帧在栈上构造析构（栈槽 8 字节）；Find* 只在选轨与真帧路径里（空播不可达）
HLE_FORCE("_ZN5movie9MediaDataC1Ev", "过场空播：空 MediaData", void, mv_MediaData, void **self) { *self = 0; }
HLE_FORCE("_ZN5movie9MediaDataD1Ev", "过场空播：空 MediaData", void, mv_MediaDataD, void **self) { (void)self; }
#define TRAPFN(mangled, cname, what) \
    HLE_FORCE(mangled, "过场空播：协议外（" what "）", void, cname, void) { die("movie 剪口：%s 被调用（不在空播协议内）", what); }
TRAPFN("_ZNK5movie9MediaData9FindInt32EPKcPi", mv_FindInt32, "MediaData::FindInt32")
TRAPFN("_ZNK5movie9MediaData9FindInt64EPKcPl", mv_FindInt64, "MediaData::FindInt64")
TRAPFN("_ZNK5movie9MediaData10FindStringEPKcPS2_", mv_FindString, "MediaData::FindString")
// Buffer：解码线程与取帧在游戏栈上构造（每处 ≥0x40 字节），宿主布局 mvbuf
HLE_FORCE("_ZN5movie6BufferC1Ev", "过场空播：空 Buffer", void, mv_Buffer, mvbuf *b) { memset(b, 0, sizeof *b); }
HLE_FORCE("_ZN5movie6BufferD1Ev", "过场空播：Buffer 无资源", void, mv_BufferD, mvbuf *b) { (void)b; }
HLE_FORCE("_ZNK5movie6Buffer4DataEv", "过场空播：Buffer 数据指针", void *, mv_BufData, const mvbuf *b) { return b->data; }
HLE_FORCE("_ZNK5movie6Buffer6OffsetEv", "过场空播：Buffer 偏移", int, mv_BufOffset, const mvbuf *b) { return b->off; }
HLE_FORCE("_ZNK5movie6Buffer4SizeEv", "过场空播：Buffer 长度", int, mv_BufSize, const mvbuf *b) { return b->size; }
HLE_FORCE("_ZN5movie6Buffer8SetRangeEii", "过场空播：Buffer 设范围", void, mv_BufSetRange, mvbuf *b, int off, int size) { b->off = off; b->size = size; }
HLE_FORCE("_ZN5movie6Buffer18SetDataAndCapacityEPvm", "过场空播：Buffer 设数据", void, mv_BufSetData, mvbuf *b, void *p, uint64_t cap) { b->data = p; b->cap = cap; b->off = b->size = 0; }
