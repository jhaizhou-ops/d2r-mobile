// nn::time：真机经 time 服务取网络校准的用户时钟；宿主直接取系统时钟（单机、离线）。
// 布局按公开 Horizon ABI：PosixTime = int64 秒；CalendarTime = {s16 年, s8 月/日/时/分/秒}；
// CalendarAdditionalInfo = {u32 星期, u32 年内日, char 时区名[8], u32 夏令时, s32 UTC 偏移秒}。
// nn::time: a real device gets the network-calibrated user clock through the time service; the host reads the system clock directly (single machine, offline).
// Layout follows the public Horizon ABI: PosixTime = int64 seconds; CalendarTime = {s16 year, s8 month/day/hour/minute/second};
// CalendarAdditionalInfo = {u32 day of week, u32 day of year, char time zone name[8], u32 DST, s32 UTC offset in seconds}.
#include "hle.h"
#include <stdatomic.h>
#include <time.h>
#ifdef __ANDROID__
#include <stddef.h>
#endif

typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;
#ifdef __ANDROID__
_Static_assert(sizeof(CalendarTime) == 8, "Horizon CalendarTime ABI");
_Static_assert(sizeof(CalendarAdditionalInfo) == 24, "Horizon CalendarAdditionalInfo ABI");
_Static_assert(sizeof(time_t) == 8, "64-bit SDK time_t ABI");
_Static_assert(sizeof(struct tm) == 56 && offsetof(struct tm, tm_gmtoff) == 40 && offsetof(struct tm, tm_zone) == 48,
               "SDK and Bionic struct tm must have compatible layout");
#endif

// DM_FIXED_TIME=秒（确定性对拍用；真值侧对应 dm/verify/ryu/drive.py --fix-time）：
// ① 用户时钟（存档 d2s +0x20 最后游玩时间、.meta last_modified_time 的来源）恒返回该值；
// ② 建局随机数种子那一处 time()（main 0x3509a0 bl time，返回点 0x3509a4，与 drive.py 同点）返回该值。
// 本 SDK 的 libc time() 是开机时长（clock_gettime(0) → __nnmusl_ClockGetTime → GetSystemTick），不是墙钟；
// main 另有 17 处 time() 调用不动，原样转给 SDK 自己的 time()。
// DM_FIXED_TIME=seconds (for deterministic comparison; the ground-truth side counterpart is dm/verify/ryu/drive.py --fix-time):
// ① the user clock (source of the save's d2s +0x20 last-played time and .meta last_modified_time) always returns this value;
// ② the one time() at the game-creation random seed (main 0x3509a0 bl time, return point 0x3509a4, same point as drive.py) returns this value.
// This SDK's libc time() is uptime (clock_gettime(0) → __nnmusl_ClockGetTime → GetSystemTick), not wall clock;
// the other 17 time() call sites in main are left alone and forwarded unchanged to the SDK's own time().
static int64_t fixed_time(void) {
    static int64_t v = -2;
    if (v == -2) { const char *e = getenv("DM_FIXED_TIME"); v = e ? strtoll(e, 0, 0) : -1;
                   if (v >= 0) LOG("DM_FIXED_TIME=%lld：用户时钟与建局 time()（main 0x3509a4）返回定值", (long long)v); }
    return v;
}
HLE_FORCE("time", "按返回点认出建局种子那一处（DM_FIXED_TIME）；其余原样转调 SDK 的 time()", int64_t, h_time_time, int64_t *t) {
#ifndef __ANDROID__
    uint64_t lr = (uint64_t)__builtin_return_address(0);
    static int64_t (*sdk_time)(int64_t *); static uint64_t seed_ret;
#else
    static int64_t (*sdk_time)(int64_t *);
#endif
    if (!sdk_time) {
        module_t *ms = NULL;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i];
        void *f = ms ? module_sym(ms, "time") : NULL; if (!f) die("SDK 里找不到 time");
#ifndef __ANDROID__
        module_t *mm = NULL;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
        uint32_t w = mm ? *(uint32_t *)(mm->base + 0x3509a0) : 0;
        if ((w & 0xfc000000) == 0x94000000) seed_ret = (uint64_t)mm->base + 0x3509a4;               // bl time（构建 db0507f3…；改写器会改跳转目标，只认 bl 本身） | bl time (build db0507f3…; the rewriter changes the branch target, so only the bl itself is matched)
        else if (fixed_time() >= 0) LOG("【红】DM_FIXED_TIME：main 0x3509a0 不是预期的 bl time（%#x），建局 time() 未固定", w);
#endif
        sdk_time = (int64_t (*)(int64_t *))f;
    }
#ifndef __ANDROID__
    int64_t ft = fixed_time();
    if (ft >= 0 && lr == seed_ret) {
        static int once; if (!once++) LOG("DM_FIXED_TIME：建局 time() 返回 %lld", (long long)ft);
        if (t) *t = ft; return ft;
    }
#endif
    return sdk_time(t);
}

#ifdef __ANDROID__
static _Atomic int g_time_initialized;
HLE_FORCE("_ZN2nn4time10InitializeEv", "验证 Android 系统墙钟及本地时区后初始化宿主时间后端", Result, h_time_Initialize, void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) return RES_FAIL;
    time_t seconds = ts.tv_sec;
    struct tm calendar;
    if (!localtime_r(&seconds, &calendar)) return RES_FAIL;
    atomic_store(&g_time_initialized, 1);
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "返回 Android 时间后端的实际初始化状态", int, h_time_IsInitialized, void) { return atomic_load(&g_time_initialized); }
#else
HLE_FORCE("_ZN2nn4time10InitializeEv", "连 time 服务；时钟改取宿主系统时间", Result, h_time_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "time 服务已被接管，恒为已初始化", int, h_time_IsInitialized, void) { return 1; }
#endif
// 游戏的用户时钟（秒）：角色档头 0x20 的「存档时刻」就是序列化时取的这个值（固定时钟局写出来正是 DM_FIXED_TIME）。个人仓库翻页拿它判「这一秒里刚存过盘」
// The game's user clock (seconds): the save time at offset 0x20 of the character file header is this value, taken at serialization (with a fixed clock the written value is exactly DM_FIXED_TIME). Personal stash page turns use it to tell whether a save happened earlier in this same second
static _Atomic int64_t g_clock_shift;   // 测试开关 DM_TEST_CLOCK_AT（dm_pages.c）拨的偏移：模拟玩家改系统时间 / 网络对时 | Offset set by the test switch DM_TEST_CLOCK_AT (dm_pages.c): simulates the player changing the system time / network time sync
void dm_test_clock_shift(int64_t d) { atomic_fetch_add(&g_clock_shift, d); LOG("测试 DM_TEST_CLOCK_AT：用户时钟拨 %+lld 秒（累计 %+lld）", (long long)d, (long long)atomic_load(&g_clock_shift)); }
int64_t dm_user_clock(void) { int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL) + atomic_load(&g_clock_shift); }
int dm_user_clock_fixed(void) { return fixed_time() >= 0; }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "用户时钟 → 宿主 time()", Result, h_time_GetCurrentTime, int64_t *out) {
#ifdef __ANDROID__
    if (!out || !atomic_load(&g_time_initialized)) return RES_FAIL;
#endif
    *out = dm_user_clock(); return RES_OK;
}
// 首领连战第 6–10 关的强制恐怖地带（dm_bossrush.c 设）：g_dm_tz_time 非 0 且未过期时，只有「恐怖地带」那几处取时钟拿到它，存档时间等其余调用照旧。
// 哪几处：返回点落在地带函数 0x58f1e0 起的那一簇（0x58f1e0–0x591440），或返回后 8 条指令内调地带日程 0x58f310（服务端 0x636b38 套地带、0x694b48 等 9 处）。
// 过期：dm_bossrush 每拍续 1 秒，本局停拍（退出 / 崩溃）后不再生效，下一局建局时取到的是真实时钟。
// Forced Terror Zone for stages 6–10 of the boss rush (set by dm_bossrush.c): when g_dm_tz_time is non-zero and not expired, only the clock reads at the "Terror Zone" sites get it; save timestamps and all other calls are unchanged.
// Which sites: the return point falls in the cluster starting at the zone function 0x58f1e0 (0x58f1e0–0x591440), or the zone schedule 0x58f310 is called within 8 instructions after the return (server side 0x636b38 applying the zone, 0x694b48 and so on, 9 sites).
// Expiry: dm_bossrush renews it by 1 second every tick; once this run stops ticking (exit / crash) it no longer applies, and the next run gets the real clock when the game is created.
_Atomic int64_t g_dm_tz_time;
_Atomic uint64_t g_dm_tz_live_ns;
#ifndef __ANDROID__
static int tz_site(uint64_t lr) {   // 只在强制期间才判，每次读返回点后 8 条指令，不缓存（多线程都会取时钟） | Checked only while forcing is active, reading the 8 instructions after the return point each time, no caching (several threads read the clock)
    static _Atomic uint64_t base; uint64_t b = atomic_load(&base);
    if (!b) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; atomic_store(&base, b); }
    if (!b || lr < b) return 0;
    uint64_t off = lr - b; if (off > 0x58f1e0 && off < 0x591440) return 1;
    for (int k = 0; k < 8; k++) { uint32_t w = *(const uint32_t *)(uintptr_t)(lr + 4 * k);
        if ((w & 0xfc000000) == 0x94000000) { int64_t imm = w & 0x3ffffff; if (imm & 0x2000000) imm -= 1 << 26; if (off + 4 * k + imm * 4 == 0x58f310) return 1; } }
    return 0;
}
#endif
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock3nowEv", "用户时钟 → 宿主 time()；首领连战强制地带时恐怖地带那几处另返回", int64_t, h_time_now, void) {
#ifdef __ANDROID__
    // No main offsets are approved for the Android build. Time is read from the
    // real system clock; Apple-only game modifications cannot redirect it here.
    if (!atomic_load(&g_time_initialized)) die("Android time backend is not initialized");
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) die("Android CLOCK_REALTIME failed");
    int64_t ft = fixed_time(); return ft >= 0 ? ft : ts.tv_sec;
#else
    int64_t tz = atomic_load(&g_dm_tz_time);
    if (tz && (uint64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC) < atomic_load(&g_dm_tz_live_ns) && tz_site((uint64_t)__builtin_return_address(0))) return tz;
    int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL);
#endif
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE",
          "SDK 实现与 now() 配套，一并接管", int64_t, h_time_to_time_t, const int64_t *tp) { return *tp; }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "连 time 服务的时区规则；改用宿主 localtime_r", Result, h_time_ToCalendarTime, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt) {
#ifdef __ANDROID__
    if (!pt || !atomic_load(&g_time_initialized)) return RES_FAIL;
#endif
    time_t t = *pt; struct tm tm; if (!localtime_r(&t, &tm)) return RES_FAIL;
    static int once; if (!once++) LOG("ToCalendarTime：首次换算 %lld → %04d-%02d-%02d %02d:%02d（时区 %s，偏移 %ld 秒）", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
// 本地时刻换算强制接宿主 libc：SDK 自带的 localtime / localtime_r / mktime 没有时区数据、恒按 UTC（宿主实测：编年史「首次发现」由
// main+0xb04d34 调 localtime 再 strftime 出，差 8 小时）。真机 Switch 的 SDK 按系统时区换算，这里跟宿主系统时区走才一致；
// 宿主 TZ 平时 = 系统时区，只在 DM_FIXED_TIME 确定性对拍时钉 UTC（main.c）。struct tm 布局两边相同（9 个 int、long 偏移、时区名指针）。
// gmtime / gmtime_r / timegm 本来就是 UTC，不接。hle_table.c 的同名直通项只在符号没有定义时才生效（SDK 有定义），所以要 HLE_FORCE
// Local time conversion is forced onto the host libc: the SDK's own localtime / localtime_r / mktime have no time zone data and always use UTC (measured on the host: the chronicle's 「首次发现」(First Discovered) is produced by
// main+0xb04d34 calling localtime and then strftime, and was off by 8 hours). The SDK on a real Switch converts by the system time zone, so only following the host system time zone here is consistent;
// the host TZ normally = the system time zone, and is pinned to UTC only for deterministic comparison with DM_FIXED_TIME (main.c). The struct tm layout is the same on both sides (9 ints, long offset, time zone name pointer).
// gmtime / gmtime_r / timegm are UTC anyway and are not taken over. The same-named pass-through entries in hle_table.c only take effect when the symbol has no definition (the SDK defines them), hence HLE_FORCE
HLE_FORCE("localtime", "本地时刻 → 宿主 localtime（跟系统时区）", struct tm *, h_localtime, const int64_t *t) { time_t v = (time_t)*t; return localtime(&v); }
HLE_FORCE("localtime_r", "本地时刻 → 宿主 localtime_r（跟系统时区）", struct tm *, h_localtime_r, const int64_t *t, struct tm *out) { time_t v = (time_t)*t; return localtime_r(&v, out); }
HLE_FORCE("mktime", "本地日历 → 绝对秒，宿主 mktime（跟系统时区）", int64_t, h_mktime, struct tm *tm) { return (int64_t)mktime(tm); }
