// Horizon 内核系统调用（装 SDK 后由 SDK 运行时触发）。只实现 P1 启动链要的：内存布局 + 堆 + 查询 + 调试输出 + 计时。
// 寄存器约定：入参 w1.. / 出参 w0=Result、x1=值（GetInfo / SetHeapSize），与公开 Horizon ABI 一致。
#include "hle_ohos_compat.h"
#include "execution_internal.h"
#include "memory_layout.h"
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

// 预留一整块客体地址空间，按 Horizon 分区（地址值直接用宿主 mmap 得到的真地址）
#define RESERVE (8ull << 30)
static uint8_t *g_as;              // ASLR 区基址 = 整块
static uint8_t *g_heap; static size_t g_heap_cur, g_heap_cap;
// 地址段整个落在客体堆当前已开放（可读写）的范围内：这种读不用陷内核核对，直接 memcpy（diag.c tk_rd 等每拍读客体内存的地方用）
int sw_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
static uint8_t *g_alias; static size_t g_alias_cap;
static uint8_t *g_stack; static size_t g_stack_cap;

static void mem_init(void) {
    if (g_as) return;
    g_as = mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_as == MAP_FAILED) die("reserve guest AS");
    // 固定划分（都在预留块内）：alias 512MB、stack 256MB、heap 其余从 2GB 处起
    g_alias = g_as + 0x10000000; g_alias_cap = 0x20000000;
    g_stack = g_as + 0x30000000; g_stack_cap = 0x10000000;
    g_heap  = g_as + 0x40000000; g_heap_cap  = RESERVE - 0x40000000;   // 约 7GB，容 D2R 3GB+ 堆
    LOG("guest AS %p +%#llx  heap %p alias %p stack %p", g_as, (unsigned long long)RESERVE, g_heap, g_alias, g_stack);
}

// 应用内存池 = Switch（4 GB 机型）给应用的 3285 MB。游戏启动（main+0x1b66160）把 QueryMemoryInfo 报的总量整块设成堆、失败就减 2 MB 重试，
// 整块交给自己的 StandardAllocator（main+0x32137a0）——所以堆大小 = 池 − 映像占用，宿主报多少它就只有多少。
// 10-03 之前这里报 3 GB 且 SetHeapSize 从不失败：堆比真机少约 150 MB，进局高峰主堆只剩约 18 MB，本机负载高时时序一偏申请 128 KB 返回空、
// 客体不判空直接写（CRASH main+0x1b36138，合入后复核 inv_tap）。
#define APP_POOL_SWITCH 0xCD500000ull
// Claude（移植 iOS 主干 2ebf4e8 hle_svc.c）：报给游戏的池 = Switch 的 3285 MB + 512 MB。游戏自己固定划走约 3.08 GB，主堆余量进局只有
// 100–180 MB；用尽时原版多处不判空直接写（iOS 公开 Issue #18）。多给的量一比一变成主堆余量，没碰过的页不计常驻内存。
// 宽屏（Mate 90 2848x1320）启动时界面 / 渲染目标更大，原 3285 MB 下启动就有申请返回空（main+0x174b018 后直接读 0 地址）。
#define APP_POOL (APP_POOL_SWITCH + (512ull << 20))
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) n += g_mod[i].size; return (n + 0x1fffff) & ~(size_t)0x1fffff; }

// 内存账（memstat.m）用：客体地址空间整块范围（未初始化时 base = NULL）
void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = RESERVE; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23 };



// 已映射区间登记（MapMemory 栈、MapPhysicalMemory alias 页），QueryMemory 据此回报；SDK 靠它找空位
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_find(uint64_t a, uint64_t n) {       // 返回 1 基下标，0 = 无重叠
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {
    pthread_mutex_lock(&g_map_lock);
    if (g_nmap >= MAX_MAP) die("映射表满");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
    pthread_mutex_unlock(&g_map_lock);
}
static void map_del(int i) { pthread_mutex_lock(&g_map_lock); g_map[i] = g_map[--g_nmap]; pthread_mutex_unlock(&g_map_lock); }

int svc_thread(int num, uint64_t *regs);   // hle_thread.c：线程与同步类

static uint64_t memory_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    { void hlecount_svc(int); hlecount_svc(num); }   // SW_HLECOUNT：svc 按号计数
    mem_init();

    { uint64_t svc_host_call(int, uint64_t *); if (num >= 0x80 && svc_host_call(num, regs)) return regs[0]; }   // 0x80 起：剪口约定的宿主调用（hle_tact.c）
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];if(size&0x1fffff){regs[0]=0xca01;return 0;}
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD401; return 0; }
        if (size + image_bytes() > APP_POOL) { regs[0] = 0x10801; return 0; }   // 超出应用内存池：ResultLimitReached。游戏启动时按 2 MB 递减重试直到放得下（main+0x1b66184）
        int rc = 0;
        if (size > g_heap_cur) rc = mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE);
        else if (size < g_heap_cur) rc = mprotect(g_heap + size, g_heap_cur - size, PROT_NONE);
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p mprotect=%d errno=%d", size, g_heap, rc, errno);
        if(rc){regs[0]=0xd401;return 0;}
        g_heap_cur = size;
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
    case 0x02: { /* SetMemoryPermission: data mappings only, never manufacture executable memory. */
        uint64_t a=regs[0],n=regs[1],p=regs[2],b=(uintptr_t)g_as;
        if(!n||(a|n)&0xfff||a<b||n>RESERVE||a-b>RESERVE-n||(p&~3ull)) {regs[0]=0xdc01;return 0;}
        regs[0]=mprotect((void*)a,n,((p&1)?PROT_READ:0)|((p&2)?PROT_WRITE:0))?0xd401:0;return 0;
    }
    case 0x03: xblr_execution_stop(XBLR_EXEC_UNSUPPORTED_SVC); /* no silent ignored attribute */
    case 0x04: {  // MapMemory(dst, src, size)：SDK 建线程时把堆上的栈搬进栈区
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        if(!size||((dst|src|size)&0xfff)||src<(uintptr_t)g_heap||size>g_heap_cur||src-(uintptr_t)g_heap>g_heap_cur-size){regs[0]=0xdc01;return 0;}
        if (dst < (uintptr_t)g_stack || size>g_stack_cap || dst-(uintptr_t)g_stack>g_stack_cap-size || map_find(dst, size)) {
            LOG("MapMemory 目标非法或已占用 %#llx+%#llx", dst, size); regs[0] = 0xDC01; return 0; }
        // 苹果页 16K、Horizon 页 4K：可访问区扩到 16K 边界（多放开的是同区未用页，不影响语义）
        uint64_t lo = dst & ~0xFFFull, hi = (dst + size + 0xFFF) & ~0xFFFull;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        memcpy((void *)dst, (void *)src, size);   // 搬移语义：映射期间源区不被访问（Horizon 把它设为不可访问）
        if(mprotect((void*)src,size,PROT_NONE))die("MapMemory source protection");
        map_add(dst, size, src, 0x0B /*MappedMemory*/);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", dst, src, size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size)：内容搬回源区；16K 页可能与相邻映射共用，不收回权限
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        if(!size||((dst|src|size)&0xfff)||src<(uintptr_t)g_heap||size>g_heap_cur||src-(uintptr_t)g_heap>g_heap_cur-size){regs[0]=0xdc01;return 0;}
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            LOG("UnmapMemory 与映射不符 %#llx+%#llx", dst, size); regs[0] = 0xD401; return 0; }
        if(mprotect((void*)src,size,PROT_READ|PROT_WRITE))die("UnmapMemory source protection");
        memcpy((void *)src, (void *)dst, size);
        if(mprotect((void*)dst,size,PROT_NONE))die("UnmapMemory destination protection");
        map_del(k - 1);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        int k;
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)g_heap; size=g_heap_cur; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (a >= g_as && a < g_as + RESERVE) {   // 空闲：到相邻映射/堆边界为止
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + RESERVE;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else {
            pthread_attr_t at;void *stack;size_t stack_size;
            if(pthread_getattr_np(pthread_self(),&at))xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);
            int e=pthread_attr_getstack(&at,&stack,&stack_size);pthread_attr_destroy(&at);
            if(!e&&addr>=(uintptr_t)stack&&addr-(uintptr_t)stack<stack_size) {base=(uintptr_t)stack;size=stack_size;state=0xb;perm=3;}
            else {extern int xblr_query_module_memory(uint64_t,uint64_t*,uint64_t*,uint64_t*);
                if(xblr_query_module_memory(addr,&base,&size,&perm))state=3;
                else {
                    xblr_address_range ranges[MAX_MOD+2];size_t nr=0;
                    ranges[nr++]=(xblr_address_range){(uintptr_t)g_as,RESERVE};
                    if(!e)ranges[nr++]=(xblr_address_range){(uintptr_t)stack,stack_size};
                    for(int i=0;i<g_nmod;++i)ranges[nr++]=(xblr_address_range){(uintptr_t)g_mod[i].base,g_mod[i].size};
                    if(xblr_free_region(addr,ranges,nr,&base,&size,&state)!=1)xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);
                    perm=0;
                }
            }
        }
        if (mi) { uint64_t *m=(uint64_t*)mi; m[0]=base; m[1]=size; ((uint32_t*)mi)[4]=state; ((uint32_t*)mi)[5]=0; ((uint32_t*)mi)[6]=perm; ((uint32_t*)mi)[7]=0; ((uint32_t*)mi)[8]=0; }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size)：只许在 alias 区内（SDK 的 VammManager 用）
        uint8_t *a = (uint8_t *)regs[0]; size_t size = regs[1];
        if (!size||((uintptr_t)a|size)&0xfff||(uintptr_t)a<(uintptr_t)g_alias||size>g_alias_cap||(uintptr_t)a-(uintptr_t)g_alias>g_alias_cap-size) { LOG("SVC %#x 越出 alias 区 %p+%#zx", num, a, size); regs[0] = 0xD401; return 0; }
        int map=map_find((uintptr_t)a,size);
        if(num==0x2c&&map){regs[0]=0xdc01;return 0;}
        if(num==0x2d&&(!map||g_map[map-1].base!=(uintptr_t)a||g_map[map-1].size!=size)){regs[0]=0xdc01;return 0;}
        int rc = num == 0x2C ? mprotect(a, size, PROT_READ | PROT_WRITE)
                             : (madvise(a, size, MADV_DONTNEED), mprotect(a, size, PROT_NONE));
        if (rc) die("SVC %#x mprotect %p+%#zx errno=%d", num, a, size, errno);
        if(num==0x2c)map_add((uintptr_t)a,size,0,0x11);else map_del(map-1);
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", num == 0x2C ? "Map" : "Unmap", a, size);
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break(reason, ptr, size)：SDK 致命中止
        uint32_t res = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        module_t *mm = module_of((void *)regs[30]);
        static int nb;
        LOG("Break#%d reason=%#llx Result=%#x svc 位于 %s+%#llx", ++nb, regs[0], res, mm ? mm->name : "?", orig_addr);
        sw_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull) return 0;   // 仅通知，可继续
        xblr_execution_stop(XBLR_EXEC_GUEST_BREAK);
    }
    case 0x1E: regs[0] = 0; regs[1] = guest_ns() * 12 / 625; return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = RESERVE;     break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = APP_POOL; break;
        case I_UsedMem:  case I_UsedNonSys:  v = g_heap_cur + 0x4000000; break;
        case I_SysResTotal: v = 0x2000000; break; case I_SysResUsed: v = 0x400000; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: { uint64_t sw_entropy64(void); v = sw_entropy64(); break; }   // SW_DETCLOCK 开时为确定流（hle_os.c）
        case I_ProgramId: v = 0x0100726014352000ull; break;
        case I_IsApplication: v = 1; break;
        case I_UserExcAddr: v = 0; break;
        default: xblr_execution_stop(XBLR_EXEC_UNSUPPORTED_SVC);
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default: xblr_execution_stop(XBLR_EXEC_UNSUPPORTED_SVC);
    }
}

/* Serialize the existing region ledger as one operation, including QueryMemory. */
static pthread_mutex_t memory_dispatch_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int memory_dispatch_owned;
/* Fatal dispatch unwinds directly to the guest thread's native stack. Release
 * only this thread's ledger lock before waking other guest waiters. */
void xblr_svc_abort_dispatch(void) {
    if(memory_dispatch_owned){memory_dispatch_owned=0;pthread_mutex_unlock(&memory_dispatch_lock);}
}
uint64_t svc_dispatch(int num,uint64_t *regs,uint64_t pc) {
    if(svc_thread(num,regs))return regs[0];
    pthread_mutex_lock(&memory_dispatch_lock);memory_dispatch_owned=1;
    uint64_t result=memory_dispatch(num,regs,pc);xblr_svc_abort_dispatch();
    return result;
}
