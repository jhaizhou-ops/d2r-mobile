// Switch 原生宿主公共头（多模块）。
// Common header of the native Switch host (multi-module).
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
// 手机产品（iOS、安卓、鸿蒙）：玩家装的包——缺省开触屏默认设置、真实声音、性能记录、3D 渲染比例等；与之相对的是 Mac 开发宿主（闸门 / 对拍缺省全关）。
// 只表示「产品缺省」；真正依赖 iOS 系统的分支（内嵌签名代码段、CoreFoundation 等）仍按 TARGET_OS_IPHONE。
// Phone product (iOS, Android, HarmonyOS): the package players install -- touch default settings, real sound, performance logging, 3D render scale etc. are on by default; the counterpart is the Mac development host (gates / A-B comparisons, all off by default).
// It only means "product defaults"; branches that truly depend on the iOS system (embedded signed code segments, CoreFoundation, etc.) still use TARGET_OS_IPHONE.
#if defined(__ANDROID__) || defined(__OHOS__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define DM_PHONE 1
#else
#define DM_PHONE 0
#endif

#define MAX_MOD 8
#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { uint8_t *d; size_t n; } dm_blob;   // 整份读进内存的一个原版存档文件（dm_pages.c dm_save_blobs） | One save file of the original game read whole into memory (dm_pages.c dm_save_blobs)
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
enum { DT_INIT = 12, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_STRTAB = 5,
       DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };

// 一个 NSO 模块的运行时状态
// Run-time state of one NSO module
typedef struct {
    char name[16];
    uint8_t *base;             // 映像（含补丁区）在宿主地址空间的首址 | Start address of the image (including the patch area) in the host address space
    size_t size;
    uint64_t text_end, ro_off, data_off, patch_off, key_page_off, key_page_size;   // 来自改写器 fixups 头 | From the rewriter fixups header
    unsigned key_idx[8];
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint32_t *symh; unsigned symh_mask;   // 已定义符号的名字哈希表（开放寻址，存符号下标；同名只留下标最小者 = 线性扫描的首个命中） | Name hash table of defined symbols (open addressing, stores symbol indices; for a duplicated name only the smallest index is kept = the first hit of a linear scan)
    uint64_t init, init_array, init_arraysz;   // init = DT_INIT（模块 _init：登记 DSO 并跑本模块 init_array） | init = DT_INIT (the module's _init: registers the DSO and runs this module's init_array)
} module_t;

extern module_t g_mod[MAX_MOD];
extern int g_nmod;
// 四个线程本地槽：K18 = Switch 的 x18、KTLS = tpidr_el0、KTLR = tpidrro_el0（ThreadLocalRegion）、KGATE = 宿主门入口；实际键号由装载器写入键页
// Four thread-local slots: K18 = the Switch's x18, KTLS = tpidr_el0, KTLR = tpidrro_el0 (ThreadLocalRegion), KGATE = host gate entry; the actual key numbers are written into the key page by the loader
extern pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
extern pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;

void load_modules(const char *dir, const char **names, int n);
void run_module_inits(void);
void *module_sym(module_t *m, const char *name);
void *guest_sym(const char *name);
module_t *module_of(const void *p);
// 门分派：改写后的 svc / 计时器读经公共入口进来，ret_addr = 该门块返回点，regs 指向保存的 x0..x30
// Gate dispatch: rewritten svc / timer reads come in through the common entry, ret_addr = the return point of that gate block, regs points to the saved x0..x30
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
// 观测门（改写器 OBS 表；编号与 patch_nso.py 一致）：只读回调，挂上即在门命中时调用
// Observation gates (rewriter OBS table; numbering matches patch_nso.py): read-only callbacks, called on a gate hit once attached
enum { OBS_UIMSG = 1, OBS_TICK = 2, OBS_C2S = 3, OBS_C2S_SYS = 4, OBS_SETLANG = 5, OBS_TARGET_RANK = 7, OBS_UI_PROJECT = 8, OBS_FREEZE_WORLD = 10, OBS_FREEZE_CLIENT = 11, OBS_FREEZE_ANIM = 12, OBS_FREEZE_RENDER = 13, OBS_TEXTBOX = 14, OBS_EVENT_MSG = 15, OBS_LEVEL_WARP = 16, OBS_QUEST_KILL = 17, OBS_NOTICE_INPUT = 18, OBS_ERR_SMOOTH = 19, OBS_MODEL_YAW = 20, OBS_TOOLTIP = 21, OBS_PRESET_SPAWN = 22, OBS_MAX = 23 };   // 6 = iOS 绑定读取（dm_touch_game_bridge.c） | 6 = iOS binding read (dm_touch_game_bridge.c)
void gate_obs_set(int id, void (*fn)(const uint64_t *regs));
int gate_obs_present(int id);
void dm_quiet_difficulty_msg(int on);   // 秘境局里拦原版「游戏难度设为 N」提示（观测门 15，diag.c） | In a rift run, block the original game's 「游戏难度设为 N」(Game difficulty set to N) message (observation gate 15, diag.c)
int gate_svc_present(int num);   // 产物里有没有这个 svc 号的门（剪口引入的宿主调用号用）   // 产物里有没有这个观测门（gates.tsv 登记过） | Whether the build output has a gate for this svc number (for host call numbers introduced by cuts)   // whether the build output has this observation gate (registered in gates.tsv)
uint64_t now_ns(void);
uint64_t guest_ns(void);              // 客体时钟（hle_os.c；DM_TICK_HOLD 冻结时不走） | Guest clock (hle_os.c; does not advance while DM_TICK_HOLD is frozen)
#ifdef _WIN32
int svc_guest_stack_limit(uint64_t sp, uint64_t *limit);
#endif
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // 按原因冻结（1 后台，2 系统键盘）；见 hle_os.c | Freeze by reason (1 background, 2 system keyboard); see hle_os.c
void dm_guest_frame_tick(void);        // 每次出图调（hle_nvn.c）：DM_DETCLOCK 开时把客体时钟推进一个固定步长 | Called on every present (hle_nvn.c): when DM_DETCLOCK is on, advances the guest clock by one fixed step
int dm_nvn_ruler_truth(void);         // 视觉尺子真值阶段（DM_NVN_RULER=1 的前两张截图）为 1 | 1 during the ground-truth phase of the visual ruler (the first two screenshots with DM_NVN_RULER=1)
uint64_t dm_nvn_frame_count(void);
uint64_t dm_nvn_sub_cpu_ns(void);     // 提交（不含出图）累计 CPU 纳秒 | Cumulative CPU nanoseconds of submit (excluding present)
int dm_ingame_now(void);              // 是否在局内（diag.c） | Whether we are in a run (diag.c)
void dm_3d_scale_feed(double iv_ms);   // 出图线程每帧报实际上屏间隔（−1 丢弃 / 0 无读数），驱动动态 3D 比例 | The present thread reports the actual present interval every frame (−1 discard / 0 no reading), driving the dynamic 3D scale
float dm_3d_scale_cur(void);          // 当前生效的 3D 渲染比例（hle_oe.c scale3d_tick）     // 当前累计出图帧数（DM_TICKS 对齐观测） | The 3D render scale currently in effect (hle_oe.c scale3d_tick)     // current cumulative count of presented frames (for DM_TICKS alignment observation)
void dm_backtrace(uint64_t fp);
void crash_watch_progress(uint32_t tick, const char *stage);
void crash_watch_frame(void);
void crash_watch_pregame_done(void);
uint64_t crash_watch_progress_count(void);
void crash_dump_now(const char *why);   // 按需导出现场（线程 + 图形等待点），不退出 | Dump state on demand (threads + graphics wait points), without exiting
// 诊断开关：main 启动时从环境变量读一次（getenv 要拿 libc 全局环境锁，进热路径会让全部客体线程排队）
// Diagnostic switches: read once from environment variables when main starts (getenv takes the libc global environment lock, and on a hot path it would make all guest threads queue up)
extern uint64_t g_t0_ns;   // 宿主启动时刻（now_ns），日志相对时间用 | Host start time (now_ns), used for relative times in the log
extern uint64_t g_proc_start_ns;   // 安卓 App 在 dm_main 前交来的进程启动时刻（now_ns 时基），0 = 未交来 | process start time handed over by the Android App before dm_main (now_ns time base), 0 = not given
extern struct dm_flags { int thr, svc, fs, movie, dbg, imports, bootprof; } g_flag;
// 启动时间线（DM_BOOTPROF=1，diag.c）：分段打点 + 计数器（纳秒 / 字节，全线程累加）；可用 PROF_GFX_* 埋点
// Boot timeline (DM_BOOTPROF=1, diag.c): segment marks + counters (nanoseconds / bytes, accumulated over all threads); PROF_GFX_* can be used for instrumentation
enum { PROF_FS_READ, PROF_WAIT_LOCK, PROF_WAIT_CV, PROF_WAIT_ADDR, PROF_WAIT_SYNC, PROF_SLEEP, PROF_MAIN_WAIT,
       PROF_GFX_PRESENT, PROF_GFX_SHADER, PROF_TACT, PROF_GFX_BCDEC, PROF_N };   // PROF_GFX_BCDEC：BC 贴图软解在调用线程上的墙钟与输出字节（马良无 BC）；PROF_TACT：TACT 块 zlib 解码（svc #0x80，宿主 libdeflate）耗时与输出字节 | PROF_TACT: time and output bytes of TACT block zlib decode (svc #0x80, host libdeflate)
void dm_prof_add(int id, uint64_t ns, uint64_t bytes);
void dm_prof_mark(const char *seg);
uint64_t dm_prof_ribbon_ns(void);   // 第一次出现 OpenPanel MainMenuButtonRibbon 的 now_ns 时刻（0 = 尚未；需 DM_BOOTPROF=1） | The now_ns time at which OpenPanel MainMenuButtonRibbon first appeared (0 = not yet; needs DM_BOOTPROF=1)
void *objmap_get(const void *guest); void objmap_put(const void *guest, void *host); void *objmap_del(const void *guest);
void hle_signal_system_event(void *g);   // hle_thread.c：触发服务层 SystemEvent（任何线程） | hle_thread.c: signal a service-layer SystemEvent (any thread)
void hle_bind_system_event(void *g, int autoclear);   // 客体线程上把客体 SystemEvent 绑到内核事件 | On a guest thread, bind a guest SystemEvent to a kernel event
// 缺页钩子 / 宿主写客体内存前钩子（crash.c；写保护脏页跟踪用）
// Page fault hook / hook before the host writes guest memory (crash.c; used for write-protect dirty page tracking)
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why));   // 资源计数回调（图形层注册）：每 250 拍、读取画面已撤、记满退出时调 | Resource count callback (registered by the graphics layer): called every 250 ticks, when the loading screen has been dismissed, and on exit when the record is full
void (*dm_res_report_cb(void))(uint32_t tick, const char *why);   // 取已注册的资源计数回调（未注册为空） | Get the registered resource count callback (null if not registered)
void dm_guest_write_prepare(void *addr, size_t len);
// 图形生命周期（hle_nvn.c）：on=1 挂起（之后提交 / 出图阻塞，返回前 GPU 在途工作清空），on=0 恢复；iOS 前端在进出后台时调用
// Graphics lifecycle (hle_nvn.c): on=1 suspends (afterwards submit / present block, and in-flight GPU work is drained before returning), on=0 resumes; called by the iOS front end when entering and leaving the background
void dm_gfx_suspend(int on);
void dm_gfx_shot_at(long k, const char *path);
void dm_shot_written(const char *path, int ok);   // 图形侧截图写盘完成（diag.c：DM_TICK_HOLD=E:SHOT 据此解冻） | The graphics side finished writing a screenshot to disk (diag.c: DM_TICK_HOLD=E:SHOT unfreezes on this)
void dm_host_shutdown(const char *why);   // 优雅退出（crash.c）：停后台线程、音频、图形清空后 _exit(0)；SIGTERM 与 iOS App 终止回调用   // 图形侧：当前出图帧之后第 k 帧截图（nvn_mtl.m） | Graceful exit (crash.c): stop background threads and audio, drain graphics, then _exit(0); used by SIGTERM and the iOS App termination callback   // graphics side: screenshot of the k-th frame after the current presented frame (nvn_mtl.m)
