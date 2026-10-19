// 客体内存写保护脏页跟踪：预排布贴图（CPU 写、GPU 读）的池页上传后设只读，游戏一写即缺页 → 标脏、解保护；
// 使用前查覆盖页的写代号，比上传时新就重传。NVN 语义：贴图描述符指向池内存，GPU 每次读的是当前内容
// （实测游戏会把流式素材写进仍被采样的贴图存储，不重新登记）。
// 信号处理约束：缺页路径只用原子操作与 mprotect，不分配、不加锁。
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "nvn_dirty_summary.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __ANDROID__
#include "../../android/nvn_platform_clock.h"
#include <errno.h>
#else
#include <mach/mach_time.h>
#endif
#include <sched.h>

// 页状态与 mprotect 的次序（改保护的一方先把状态改成 BUSY，mprotect 落地后才改成终态；别的线程见 BUSY 就等）：
//   设只读：NONE → BUSY → mprotect(只读) → PROT；解保护：PROT → BUSY → mprotect(可写) → NONE。
// 不变式：状态是 NONE 的页一定可写、是 PROT 的页一定只读。原先设只读是「先记 PROT、攒一段再 mprotect」，这段时间里另一线程的写前解保护
// 会把状态改回 NONE，随后攒批的 mprotect 落地 ⇒ 页只读而状态 NONE：游戏线程写它时缺页处理只回「重执行」永远空转；
// 宿主用系统调用往里读文件时 prep_write 看状态 NONE 跳过，系统调用不走缺页处理，直接 EFAULT、数据读不全。
// BUSY 最长持续 = 一次 mprotect 的时间（设只读时再加上给这一段逐页改状态的几次原子写）。等待有上限 BUSY_WAIT_MS，超限报红：
// 缺页处理里超限按「不是我们的页」返回，交给崩溃处理如实报崩，不静默空转。等待只读原子量、不拿任何锁。
// Order of page state and mprotect (the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY):
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, and one whose state is PROT is always read-only. Setting read-only used to be "record PROT first, batch up a run, then mprotect"; during that time another thread's unprotect-before-write
// could set the state back to NONE, and then the batched mprotect landed => the page read-only with state NONE: when the game thread wrote to it the page-fault handler only returned "re-execute" and spun forever;
// when the host read a file into it with a system call, prep_write saw state NONE and skipped, the system call did not go through the page-fault handler and failed with EFAULT, leaving the data incomplete.
// BUSY lasts at most one mprotect (plus, when setting read-only, the few atomic writes that change the state of the pages in the run). The wait has an upper limit BUSY_WAIT_MS and reports red beyond it:
// in the page-fault handler, exceeding it returns "not our page" and lets the crash handler report the crash truthfully instead of spinning silently. The wait only reads atomics and takes no lock.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 4096   // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; NvnDirtySummary summary; } range_t;
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_summary_enabled,g_query_recording;
static _Atomic uint64_t g_query_calls,g_query_pages,g_query_blocks,g_query_skipped,g_query_fast;
static _Atomic int g_live, g_live_peak;   // 在册的池数与峰值（内存读数行用） | number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 销池（客体线程，可能并发）互斥选槽；缺页处理路径不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模块的 mprotect：调用次数、覆盖页数、耗时（mach 时基，缺页路径里也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static uintptr_t address_of(const void *p) {
#ifdef __ANDROID__
    /* Bionic heap pointers may carry a top-byte tag. Linux signal si_addr and
     * page-protection bookkeeping describe the untagged virtual address. */
    return (uintptr_t)p & UINT64_C(0x00ffffffffffffff);
#else
    return (uintptr_t)p;
#endif
}
static void mp(uintptr_t a, size_t len, int prot) {
#ifdef __ANDROID__
    uint64_t t=dm_android_nvn_clock_ns();
    if(mprotect((void *)a,len,prot)) {
        /* This path can run in a signal handler: no stdio/allocator/locks. */
        static const char failure[]="[nvn-android] fatal dirty-page mprotect failure\n";
        ssize_t written=write(STDERR_FILENO,failure,sizeof(failure)-1);(void)written;_Exit(139);
    }
    atomic_fetch_add(&g_mp_ticks,dm_android_nvn_clock_ns()-t);atomic_fetch_add(&g_mp_calls,1);atomic_fetch_add(&g_mp_pages,len>>g_shift);
#else
    uint64_t t = mach_absolute_time(); mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
#endif
}

#ifdef __ANDROID__
static uint64_t dirty_now_ns(void) { return dm_android_nvn_clock_ns(); }
#else
static mach_timebase_info_data_t g_tb;   // 首次登记池时取好（缺页路径里只做乘除） | Obtained when a pool is first registered (the fault path only multiplies and divides)
static uint64_t dirty_now_ns(void) { if (!g_tb.denom) mach_timebase_info(&g_tb); return mach_absolute_time() * g_tb.numer / g_tb.denom; }
#endif
static _Atomic int g_busy_red;
// 等这一页离开 BUSY。返回 1 = 已离开；0 = 等过了上限（报红一次）。信号处理里可用：只有原子读、mach_absolute_time、sched_yield
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = dirty_now_ns(); if (!t0) t0 = now;
        if (now - t0 > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("【红】脏页跟踪：页 %p 停在「忙」超过 %d 毫秒（改保护的线程没走完），不再等", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
/* find() 缓存：脏检查每帧上万次（缓冲绑定 9,627 次/帧），而 g_r 是定长数组、指针稳定。
 * 直接映射缓存只需校验 live 与地址范围，把绝大多数调用变成 O(1)，不再线性扫全表。 */
/* find() cache: dirty checks run tens of thousands of times per frame (9,627 buffer bindings per
 * frame) and g_r is a fixed array with stable pointers, so a direct-mapped cache validated against
 * live + range turns almost every call into O(1) instead of a linear scan. */
static _Thread_local struct { uintptr_t base, end; range_t *r; } g_fc[16];
static uint64_t g_find_calls, g_find_hits, g_find_steps;
void nvn_dirty_find_stats(uint64_t out[3]) { out[0]=g_find_calls; out[1]=g_find_hits; out[2]=g_find_steps; g_find_calls=g_find_hits=g_find_steps=0; }
static range_t *find(uintptr_t a) {
    unsigned h = (unsigned)(((a >> 20) ^ (a >> 28)) & 15);
    if (g_fc[h].r && atomic_load_explicit(&g_fc[h].r->live, memory_order_acquire) &&
        a >= g_fc[h].base && a < g_fc[h].end) { g_find_hits++; return g_fc[h].r; }
    int n = atomic_load(&g_nr);
    g_find_calls++;
    for (int i = 0; i < n; i++) {
        g_find_steps++;
        if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= g_r[i].base && a < g_r[i].end) {
            g_fc[h].base = g_r[i].base; g_fc[h].end = g_r[i].end; g_fc[h].r = &g_r[i];
            return &g_r[i];
        }
    }
    return NULL;
}
static void store_generation(range_t *r,uintptr_t page) {
    uint64_t generation=atomic_fetch_add(&g_now,1)+1;
    if(g_summary_enabled)nvn_dirty_summary_store(&r->summary,r->gen,page,generation);
    else atomic_store(&r->gen[page],generation);
}
static void unprotect_page(range_t *r, uintptr_t p) {   // 调用方已把状态从 PROT 改成 BUSY | The caller has already changed the state from PROT to BUSY
    mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    store_generation(r,p);
    atomic_store(&r->state[p], PG_NONE);
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    uintptr_t address=address_of(addr);
    range_t *r = is_write ? find(address) : NULL;
    if (!r) return 0;
    uintptr_t p = (address - r->base) >> g_shift;
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_faults, 1); return 1; }
    if (st == PG_BUSY) return busy_wait(r, p);   // 别的线程正在改这页的保护：等它改完重执行（改成了只读就再缺页一次、走上面那一支） | Another thread is changing this page's protection: wait for it to finish and re-execute (if it became read-only, it faults once more and takes the branch above)
    return atomic_load(&r->gen[p]) != 0;   // 刚被别的线程解掉（本次缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = address_of(addr), e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift;
        while (p <= q) {
            uint8_t st = PG_PROT;
            if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) {
                // 批量快路径（dodola 版思路）：写回常覆盖一整段受保护的连续页，把相邻页一起认领，
                // 用一次 mprotect 覆盖整段；页序与逐页记账不变，只有保护系统调用被合并。
                // Batched fast path (dodola's idea): a GPU writeback often covers a long protected
                // run. Claim adjacent pages together so one mprotect replaces one per page. Per-page
                // ordering is kept; only the protection syscall is batched.
                uintptr_t after = p + 1;
                while (after <= q) { st = PG_PROT;
                    if (!atomic_compare_exchange_strong(&r->state[after], &st, PG_BUSY)) break;
                    after++; }
                size_t count = after - p;
                mp(r->base + (p << g_shift), count << g_shift, PROT_READ | PROT_WRITE);
                for (uintptr_t page = p; page < after; page++) { store_generation(r, page); atomic_store(&r->state[page], PG_NONE); }
                atomic_fetch_add(&g_preps, count);
                p = after;
            } else {
                // 竞争页保留主线的语义：BUSY 等完要重看：对方可能是在设只读，等完这页是 PROT，照样得解 | Contested page keeps the mainline semantics: after waiting out BUSY, look again; the other side may have been setting read-only, in which case the page is PROT after the wait and still has to be unprotected
                if (st == PG_BUSY && busy_wait(r, p)) continue;
                p++;
            }
        }
        a = stop;
    }
}

static void dirty_initialize(void) {
    const char *summary=getenv("DM_DIRTY_SUMMARY"),*stats=getenv("DM_DIRTY_QUERY_STATS");
    g_summary_enabled=summary&&atoi(summary)!=0;g_query_recording=stats&&atoi(stats)!=0;
    g_pg=(uintptr_t)getpagesize();g_shift=__builtin_ctzl(g_pg);dm_fault_hook_register(on_fault);dm_guest_write_prep_register(prep_write);
}
void nvn_dirty_add_range(const void *mem, size_t size) {
    static pthread_once_t once=PTHREAD_ONCE_INIT;if(pthread_once(&once,dirty_initialize))die("dirty tracker initialization failed");
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】脏页跟踪区间已满（%d），池 %p 不跟踪", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t address=address_of(mem);
    if(!mem || !size || size>UINTPTR_MAX-address-(g_pg-1))die("dirty tracker range overflow");
    uintptr_t base = address & ~(g_pg - 1), end = (address + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("脏页表分配失败（%zu 页）", np);
        // 旧数组不 free：信号处理里可能仍有极短暂的陈旧读者，泄漏量 = 每页 9 字节。
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    r->base = base; r->end = end; r->state = state; r->gen = gen;
    if(g_summary_enabled){
        size_t blocks=(np+NVN_DIRTY_BLOCK_PAGES-1)>>NVN_DIRTY_BLOCK_SHIFT;
        if(r->summary.capacity<blocks){
            _Atomic uint64_t *next=calloc(blocks,sizeof(*next));if(!next)die("dirty summary allocation");
            /* Like gen/state, old storage remains available to stale signal readers. */
            r->summary.blocks=next;r->summary.capacity=blocks;
        }else for(size_t block=0;block<blocks;block++)atomic_store(&r->summary.blocks[block],0);
        atomic_store(&r->summary.newest,0);
    }
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// 池销毁：先把仍受保护的页还原成可写（池内存归还游戏后可能另作他用），再标失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find(address_of(mem));
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    uintptr_t a=address_of(addr);range_t *r = find(a); return r && len<=r->end-a;
}
// 设只读：先把页标成 BUSY，mprotect 落地后才标 PROT（次序与不变式见文件头的状态说明）
// Set read-only: mark the page BUSY first, and mark it PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    range_t *r = find(address_of(addr));
    uintptr_t a = address_of(addr), p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // 连续置成 PROT 的页合成一次 mprotect（逐页调用在首传大贴图时一帧上千次；选角页启动帧 5：4888 次）。
    // 已是 PROT / BUSY 的页断开连续段：不重复保护，也不碰别的线程正在改的页
    // Consecutive pages set to PROT are merged into one mprotect (per-page calls ran to over a thousand per frame when large textures were first uploaded; character select, startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break a run: they are not protected again, and pages another thread is changing are left alone
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ); for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT); open = 0; }
    }
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    int recording=atomic_load(&g_query_recording);
    if(recording)atomic_fetch_add(&g_query_calls,1);
    range_t *r = find(address_of(addr));
    if(!r || len>r->end-address_of(addr))return 1; /* Untracked/partly tracked stays conservatively dirty. */
    uintptr_t a = address_of(addr), p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    NvnDirtyQueryStats stats={0};int dirty=0;
    if(q-p>=4&&atomic_load(&g_summary_enabled))dirty=nvn_dirty_summary_since(&r->summary,r->gen,p,q,stamp,recording?&stats:NULL);
    else for(uintptr_t i=p;i<=q;i++){
        if(recording)stats.page_reads++;
        if(atomic_load(&r->gen[i])>stamp){dirty=1;break;}
    }
    if(recording){
        atomic_fetch_add(&g_query_pages,stats.page_reads);atomic_fetch_add(&g_query_blocks,stats.block_reads);
        atomic_fetch_add(&g_query_skipped,stats.skipped_pages);atomic_fetch_add(&g_query_fast,stats.range_fast_paths);
    }
    return dirty;
}
int nvn_dirty_next_range(const void *addr,size_t len,uint64_t stamp,size_t *cursor,size_t *offset,size_t *range_len) {
    if(!len || !cursor || !offset || !range_len || *cursor>=len)return 0;
    size_t from=*cursor;
    if(!nvn_dirty_tracked(addr,len)) {
        *offset=from;*range_len=len-from;*cursor=len;return 1;
    }
    uintptr_t a=address_of(addr),request=a+from,end=a+len;
    range_t *r=find(a);
    if(!r)return 0;
    uintptr_t first=(request-r->base)>>g_shift,last=(end-1-r->base)>>g_shift;
    uintptr_t dirty_first=0,dirty_after=0;int active=0;
    for(uintptr_t p=first;p<=last;p++) {
        if(atomic_load(&r->gen[p])>stamp) {
            if(!active){dirty_first=p;active=1;}
            dirty_after=p+1;
        } else if(active) break;
    }
    if(!active){*cursor=len;return 0;}
    uintptr_t begin=r->base+(dirty_first<<g_shift),finish=r->base+(dirty_after<<g_shift);
    if(begin<request)begin=request;
    if(finish>end)finish=end;
    *offset=begin-a;*range_len=finish-begin;*cursor=*offset+*range_len;return 1;
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_query_stats(NvnDirtyQueryStats *stats,int *summary_enabled,int *recording) {
    *stats=(NvnDirtyQueryStats){atomic_load(&g_query_calls),atomic_load(&g_query_pages),atomic_load(&g_query_blocks),atomic_load(&g_query_skipped),atomic_load(&g_query_fast)};
    *summary_enabled=g_summary_enabled;*recording=g_query_recording;
}
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
#ifdef __ANDROID__
    *calls=atomic_load(&g_mp_calls);*pages=atomic_load(&g_mp_pages);*ns=atomic_load(&g_mp_ticks);
#else
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
#endif
}
