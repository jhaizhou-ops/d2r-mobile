// 客体线程环境：门槽 = 宿主门入口；x18 槽清零；tpidr_el0 槽 = ELF TLS 块；tpidrro_el0 槽 = Horizon ThreadLocalRegion（0x200）。
// SDK 的内部临界区从 TLR[0x1f8] 取当前线程对象指针，这里给每线程分配一个零初始化对象并写入。
// Guest thread environment: gate slot = host gate entry; x18 slot cleared; tpidr_el0 slot = ELF TLS block; tpidrro_el0 slot = Horizon ThreadLocalRegion (0x200).
// The SDK's internal critical section takes the current thread object pointer from TLR[0x1f8]; here each thread gets a zero-initialized object allocated and written in.
#include "dm.h"
#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400
extern void dm_gate(void);            // entry.S：门公共入口 | entry.S: common gate entry
#include "front.h"
#include "dm_uiscreen.h"
#ifndef __ANDROID__
static pthread_key_t g_keyown;   // 本线程申请到的 TLS 块 / 线程本地区的原始指针（见函数末尾） | raw pointers of the TLS block / thread-local region allocated by this thread (see the end of the function)
static pthread_once_t g_keyown_once = PTHREAD_ONCE_INIT;
static void own_free(void *p) { void **own = p; free(own[0]); free(own[1]); free(own); }
static void own_key_init(void) { if (pthread_key_create(&g_keyown, own_free)) die("创建线程本地块的释放键失败"); }
#else
// Android: thread_slots.c's guest-slot context owns and frees the TLS / TLR / thread-object blocks.
#endif
void guest_thread_init(void) {
    pthread_setspecific(g_keygate, (void *)dm_gate);
    for (int i = 0; i < 2; i++) if (g_keyres[i]) pthread_setspecific(g_keyres[i], (void *)dm_game_res(i));   // 分辨率参数位：{u32 宽, u32 高} 打包 | resolution parameter slot: {u32 width, u32 height} packed
    if (g_keyaspect || g_keyaspectui) {   // 宽高比上限参数位：max(19/9, 两种模式输出宽 / 高)，float 位模式放低 32 位 | aspect ratio limit parameter slot: max(19/9, output width / height of the two modes), float bit pattern in the low 32 bits
        float a = 19.0f / 9.0f; int wide = 0;
        for (int i = 0; i < 2; i++) { uint64_t r = front_game_res(i); float w = (float)(uint32_t)r, h = (float)(uint32_t)(r >> 32); if (h > 0 && w / h > a) { a = w / h; wide = 1; } }
        // 界面边柱着色器的上限：屏幕宽于 19:9 时另加 0.15，把边柱向内的柔边推出屏幕；16:9 不变。
        // 推导：边柱内缘 x = 屏高 × 上限 / 2，上限每加 Δ，内缘每侧外移 屏高 × Δ / 2。Mac 1952x900 实测柔边宽度 ≤ 30 像素
        // （上限 = 屏幕比例时右缘第 0–20 列由黑渐亮，上限推到 2.4 即消失）⇒ 需 Δ ≥ 2 × 30 / 900 ≈ 0.067；取 0.15（每侧外移约 67 像素）留出一倍以上余量。
        // 不是魔数：屏高变大时余量按比例不变（Δ 是相对屏高的量）
        // Limit of the UI side-pillar shader: when the screen is wider than 19:9 add another 0.15, pushing the pillars' inward soft edge off the screen; 16:9 is unchanged.
        // Derivation: pillar inner edge x = screen height * limit / 2, so for every delta added to the limit the inner edge moves outward by screen height * delta / 2 on each side. Measured on Mac 1952x900, the soft edge is <= 30 pixels wide
        // (with limit = screen aspect ratio, columns 0–20 at the right edge fade from black to bright, and it disappears once the limit is pushed to 2.4) => need delta >= 2 * 30 / 900 ~ 0.067; 0.15 is taken (about 67 pixels outward per side), leaving more than double the margin.
        // Not a magic number: when the screen height grows the margin stays proportional (delta is relative to screen height)
        float ui = wide ? a + 0.15f : a;
        uint32_t bits;
        if (g_keyaspect) { memcpy(&bits, &a, 4); pthread_setspecific(g_keyaspect, (void *)(uintptr_t)bits); }
        if (g_keyaspectui) { memcpy(&bits, &ui, 4); pthread_setspecific(g_keyaspectui, (void *)(uintptr_t)bits); }
    }
    pthread_setspecific(g_key18, 0);
#ifdef __ANDROID__
    // Android: guest TLS / thread-local region / thread object are three separate allocations.
    // thread_slots.c records them when the signal-safe guest-slot context attaches and its key
    // destructor frees them at thread exit -- so they must NOT also be routed through g_keyown
    // (the merged mainline scheme bundles them into one block and frees interior pointers, which
    // free() aborts on).
    {
        uint8_t *tlr = calloc(1, TLR_SIZE);
        *(void **)(tlr + 0x1f8) = calloc(1, THREAD_OBJ_SIZE);   // 当前线程对象 | current thread object
        pthread_setspecific(g_keytls, calloc(1, TLS_SIZE));
        pthread_setspecific(g_keytlr, tlr);
        dm_android_slots_sync();
    }
#else
    // 这两块随线程退出释放。客体会改写 TLS / 线程本地区这两把键的槽（它自己设线程指针），所以不能按键里的当前值去 free：
    // 申请到的原始指针另记在本文件自己的键 g_keyown 上，由它的析构放（键析构在 pthread_exit 时调用，同信号备用栈 crash.c altstack_free；
    // 析构里只有 free，客体线程的小栈上也安全）。原先不放：游戏频繁起短命线程，每条漏 8192 + 0x200 + 0x400 = 9728 字节。
    // 线程本地区与当前线程对象并成一块（TLR[0x1f8] 那一格 SDK 也可能改写）
    // These two blocks are freed when the thread exits. The guest overwrites the slots of the TLS / thread-local-region keys (it sets its own thread pointer), so they cannot be freed by the current value in the key:
    // the raw pointers that were allocated are recorded separately on this file's own key g_keyown and freed by its destructor (key destructors are called at pthread_exit, same as the signal alternate stack, crash.c altstack_free;
    // the destructor only calls free, which is safe on the small stack of a guest thread as well). They used not to be freed: the game starts short-lived threads frequently, and each leaked 8192 + 0x200 + 0x400 = 9728 bytes.
    // The thread-local region and the current thread object are merged into one block (the slot TLR[0x1f8] may also be overwritten by the SDK)
    void **own = calloc(2, sizeof *own); uint8_t *tlr = calloc(1, TLR_SIZE + THREAD_OBJ_SIZE);
    own[0] = calloc(1, TLS_SIZE); own[1] = tlr;
    *(void **)(tlr + 0x1f8) = tlr + TLR_SIZE;               // 当前线程对象 | current thread object
    pthread_once(&g_keyown_once, own_key_init); pthread_setspecific(g_keytls, own[0]); pthread_setspecific(g_keytlr, tlr); pthread_setspecific(g_keyown, own);
#endif
}
