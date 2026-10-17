// 触屏（jhz 10-02 令接入）：前端整组推触点（sw_touch_set，front.h），游戏每帧经 nn::hid::GetTouchScreenStates 读（hle_hid.c → touch_fill）。
// 游戏的读法（main 0x1b65260）：清 16 份状态、要 16 份，只看第 0 份——触点数与各触点的 finger_id / x / y；面板坐标系 1280x720
// （它在同一函数里写常量 0x2d0_00000500 = 720 << 32 | 1280）。所以：
//   · 前端归一化坐标 [0,1)（出图画面左上为原点）→ x = nx·1280、y = ny·720，夹到 [0,1279] / [0,719]；
//   · 短按锁存：游戏只看最新一份，两次读之间按下又抬起的手指若不锁存会整个丢掉——本表每根手指先以 Start 报一次，
//     抬起后再以 End 报一次才释放（两次读之间按下又抬起 = 这次报 Start、下次报 End）。
// TouchState（0x28 字节，= libnx HidTouchState）：+0 delta_time（ns，i64）、+8 属性（bit0 Start / bit1 End）、+0xc finger_id、
// +0x10 x、+0x14 y、+0x18 直径 x、+0x1c 直径 y、+0x20 转角、+0x24 保留。TouchScreenState<16>：+0 采样号、+8 触点数、+0x10 起 16 个 TouchState。
#include "hle_ohos_compat.h"
typedef xblr_touch sw_touch;
#include <pthread.h>
enum { F_EMPTY, F_DOWN, F_LIFTED };
static struct { int st, reported, again; float x, y, ax, ay; } g_f[16];   // again/ax/ay：End 还没报出去时同 id 又按下（下次读先报 End 再报新 Start）
static pthread_mutex_t g_tl = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_tsamp; static uint64_t g_tlast_ns;

void sw_touch_set(const sw_touch *t, int n) {
    pthread_mutex_lock(&g_tl);
    int seen[16] = {0};
    for (int i = 0; i < n; i++) {
        uint32_t id = t[i].id; if (id >= 16) continue;
        seen[id] = 1;
        if (g_f[id].st == F_LIFTED && g_f[id].reported) { g_f[id].again = 1; g_f[id].ax = t[i].x; g_f[id].ay = t[i].y; continue; }
        if (g_f[id].st == F_EMPTY) { g_f[id].st = F_DOWN; g_f[id].reported = 0; }
        else if (g_f[id].st == F_LIFTED) g_f[id].st = F_DOWN;              // 抬起还没报过又按下：当作一直按着
        g_f[id].x = t[i].x; g_f[id].y = t[i].y;
    }
    for (int id = 0; id < 16; id++) {
        if (!seen[id] && g_f[id].st == F_DOWN) g_f[id].st = F_LIFTED;
        if (!seen[id] && g_f[id].again) g_f[id].again = 0;                   // 重按后又抬起（End 都还没报）：重按丢掉，仍按原抬起报 End
    }
    pthread_mutex_unlock(&g_tl);
}

static int32_t clampi(float v, int hi) { int32_t r = (int32_t)(v * (float)(hi + 1)); return r < 0 ? 0 : r > hi ? hi : r; }
// 填一份 TouchScreenState<16>（out 按 0x290 字节）；now_ns 用于 delta_time。返回写入的触点数
int touch_fill(void *out, uint64_t now_ns) {
    uint8_t *o = out; memset(o, 0, 0x290);
    pthread_mutex_lock(&g_tl);
    int64_t samp = ++g_tsamp; int64_t dt = g_tlast_ns ? (int64_t)(now_ns - g_tlast_ns) : 0; g_tlast_ns = now_ns;
    int k = 0;
    for (int id = 0; id < 16; id++) {
        if (g_f[id].st == F_EMPTY) continue;
        uint32_t attr = 0;
        if (!g_f[id].reported) { attr |= 1; g_f[id].reported = 1; }          // Start（本次第一次报）
        else if (g_f[id].st == F_LIFTED) attr |= 2;                         // End（已报过 Start、已抬起）
        uint8_t *s = o + 0x10 + 0x28 * k++;
        int32_t x = clampi(g_f[id].x, 1279), y = clampi(g_f[id].y, 719), d = 30, zero = 0, fid = id;
        memcpy(s + 0x00, &dt, 8); memcpy(s + 0x08, &attr, 4); memcpy(s + 0x0c, &fid, 4);
        memcpy(s + 0x10, &x, 4); memcpy(s + 0x14, &y, 4); memcpy(s + 0x18, &d, 4); memcpy(s + 0x1c, &d, 4); memcpy(s + 0x20, &zero, 4);
        if (attr & 2) {                                                      // End 报过：释放；若期间同 id 又按下，转成新的按下
            if (g_f[id].again) { g_f[id].st = F_DOWN; g_f[id].reported = 0; g_f[id].again = 0; g_f[id].x = g_f[id].ax; g_f[id].y = g_f[id].ay; }
            else g_f[id].st = F_EMPTY;
        }
    }
    pthread_mutex_unlock(&g_tl);
    int32_t cnt = k; memcpy(o + 0, &samp, 8); memcpy(o + 8, &cnt, 4);
    // SW_TOUCH_LOG=1（验收诊断）：非 0 触点的读逐次记一行——采样号、触点数、各点 finger_id / 面板 x,y / 属性（1 Start、2 End）；封顶 500 行
    static int lg = -1, nl; if (lg < 0) lg = getenv("SW_TOUCH_LOG") != NULL;
    if (lg && k && nl < 500) {
        char b[256]; int m = snprintf(b, sizeof b, "触屏读 采样 %lld 触点 %d", (long long)samp, k);
        for (int i = 0; i < k && m < (int)sizeof b - 40; i++) { int32_t v[4]; memcpy(v, o + 0x10 + 0x28 * i + 0x08, 16);
            m += snprintf(b + m, sizeof b - m, " | #%d (%d,%d) 属性 %d", v[1], v[2], v[3], v[0]); }
        LOG("%s", b); if (++nl == 500) LOG("SW_TOUCH_LOG 满 500 行，停记");
    }
    return k;
}
