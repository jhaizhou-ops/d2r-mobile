// nn::time：真机经 time 服务取网络校准的用户时钟；宿主直接取系统时钟（单机、离线）。
// 布局按公开 Horizon ABI：PosixTime = int64 秒；CalendarTime = {s16 年, s8 月/日/时/分/秒}；
// CalendarAdditionalInfo = {u32 星期, u32 年内日, char 时区名[8], u32 夏令时, s32 UTC 偏移秒}。
#include "hle_ohos_compat.h"
#include <stdatomic.h>
#include <time.h>

typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;

// SW_FIXED_TIME=秒（确定性对拍用；真值侧对应 sw/verify/ryu/drive.py --fix-time）：
// ① 用户时钟（存档 d2s +0x20 最后游玩时间、.meta last_modified_time 的来源）恒返回该值；
// ② 建局随机数种子那一处 time()（main 0x3509a0 bl time，返回点 0x3509a4，与 drive.py 同点）返回该值。
// 本 SDK 的 libc time() 是开机时长（clock_gettime(0) → __nnmusl_ClockGetTime → GetSystemTick），不是墙钟；
// main 另有 17 处 time() 调用不动，原样转给 SDK 自己的 time()。
static int64_t fixed_time(void) {
    static int64_t v = -2;
    if (v == -2) { const char *e = getenv("SW_FIXED_TIME"); v = e ? strtoll(e, 0, 0) : -1;
                   if (v >= 0) LOG("SW_FIXED_TIME=%lld：用户时钟与建局 time()（main 0x3509a4）返回定值", (long long)v); }
    return v;
}
HLE_FORCE("time", "按返回点认出建局种子那一处（SW_FIXED_TIME）；其余原样转调 SDK 的 time()", int64_t, h_time_time, int64_t *t) {
    uint64_t lr = (uint64_t)__builtin_return_address(0);
    static int64_t (*sdk_time)(int64_t *); static uint64_t seed_ret;
    if (!sdk_time) {
        module_t *ms = NULL, *mm = NULL;
        for (int i = 0; i < g_nmod; i++) { if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i]; if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i]; }
        void *f = ms ? module_sym(ms, "time") : NULL; if (!f) die("SDK 里找不到 time");
        uint32_t w = mm ? *(uint32_t *)(mm->base + 0x3509a0) : 0;
        if ((w & 0xfc000000) == 0x94000000) seed_ret = (uint64_t)mm->base + 0x3509a4;               // bl time（构建 db0507f3…；改写器会改跳转目标，只认 bl 本身）
        else if (fixed_time() >= 0) LOG("【红】SW_FIXED_TIME：main 0x3509a0 不是预期的 bl time（%#x），建局 time() 未固定", w);
        sdk_time = (int64_t (*)(int64_t *))f;
    }
    int64_t ft = fixed_time();
    if (ft >= 0 && lr == seed_ret) {
        static int once; if (!once++) LOG("SW_FIXED_TIME：建局 time() 返回 %lld", (long long)ft);
        if (t) *t = ft; return ft;
    }
    return sdk_time(t);
}

HLE_FORCE("_ZN2nn4time10InitializeEv", "连 time 服务；时钟改取宿主系统时间", Result, h_time_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "time 服务已被接管，恒为已初始化", int, h_time_IsInitialized, void) { return 1; }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "用户时钟 → 宿主 time()", Result, h_time_GetCurrentTime, int64_t *out) {
    int64_t ft = fixed_time(); *out = ft >= 0 ? ft : time(NULL); return RES_OK;
}
// 首领连战第 6–10 关的强制恐怖地带（sw_bossrush.c 设）：g_sw_tz_time 非 0 且未过期时，只有「恐怖地带」那几处取时钟拿到它，存档时间等其余调用照旧。
// 哪几处：返回点落在地带函数 0x58f1e0 起的那一簇（0x58f1e0–0x591440），或返回后 8 条指令内调地带日程 0x58f310（服务端 0x636b38 套地带、0x694b48 等 9 处）。
// 过期：sw_bossrush 每拍续 1 秒，本局停拍（退出 / 崩溃）后不再生效，下一局建局时取到的是真实时钟。
_Atomic int64_t g_sw_tz_time;
_Atomic uint64_t g_sw_tz_live_ns;
static int tz_site(uint64_t lr) {   // 只在强制期间才判，每次读返回点后 8 条指令，不缓存（多线程都会取时钟）
    static _Atomic uint64_t base; uint64_t b = atomic_load(&base);
    if (!b) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; atomic_store(&base, b); }
    if (!b || lr < b) return 0;
    uint64_t off = lr - b; if (off > 0x58f1e0 && off < 0x591440) return 1;
    for (int k = 0; k < 8; k++) { uint32_t w = *(const uint32_t *)(uintptr_t)(lr + 4 * k);
        if ((w & 0xfc000000) == 0x94000000) { int64_t imm = w & 0x3ffffff; if (imm & 0x2000000) imm -= 1 << 26; if (off + 4 * k + imm * 4 == 0x58f310) return 1; } }
    return 0;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock3nowEv", "用户时钟 → 宿主 time()；首领连战强制地带时恐怖地带那几处另返回", int64_t, h_time_now, void) {
    int64_t tz = atomic_load(&g_sw_tz_time);
    if (tz && (uint64_t)now_ns() < atomic_load(&g_sw_tz_live_ns) && tz_site((uint64_t)__builtin_return_address(0))) return tz;
    int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL); }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE",
          "SDK 实现与 now() 配套，一并接管", int64_t, h_time_to_time_t, const int64_t *tp) { return *tp; }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "连 time 服务的时区规则；改用宿主 localtime_r", Result, h_time_ToCalendarTime, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt) {
    time_t t = *pt; struct tm tm; if(!localtime_r(&t, &tm))return RES_FAIL;
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
