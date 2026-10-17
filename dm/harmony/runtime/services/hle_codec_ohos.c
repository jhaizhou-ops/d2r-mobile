// nn::codec::HardwareOpusDecoder：真机经 hwopus 服务用硬件解码（游戏的音频与过场音轨都是 Opus，不是联网功能）。
// 宿主：构造函数改为在同一块内存上构造 SDK 的软件 OpusDecoder（sdk 自带 libopus，ARM 原生）。
// 依据：两者同为 nn::codec::IOpusDecoder 的实现，虚表 10 个槽位顺序与签名逐一相同（析构×2、GetWorkBufferSize、IsInitialized、
// Initialize、Finalize、GetSampleRate、GetChannelCount、Reset、DecodeInterleaved）；游戏按 0x40 字节分配，软件版对象 0x30 字节。
#include "hle_ohos_compat.h"
#include <opus.h>
#include <pthread.h>
#include <unistd.h>
extern void *sw_opus_orig, *sw_opus_orig_init, *sw_opus_orig_reset; extern int sw_opus_use_host;
uint64_t sw_opus_wrap(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sw_opus_init_wrap(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sw_opus_reset_wrap(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void **g_vt_copy;
static void **vt_copy_ptr(void) { return g_vt_copy; }
#include <stdatomic.h>
#include <time.h>

HLE_FORCE("_ZN2nn5codec19HardwareOpusDecoderC1Ei", "hwopus 服务；换成 SDK 软件 OpusDecoder（同一 IOpusDecoder 虚接口）",
          void, h_codec_HwOpusCtor, void *self, int option) {
    (void)option;
    static void (*soft_ctor)(void *);
    if (!soft_ctor && !(soft_ctor = guest_sym("_ZN2nn5codec11OpusDecoderC1Ev"))) die("缺 OpusDecoder 构造");
    soft_ctor(self);
    // Claude：游戏的音乐与音效都是 Opus。真机由硬件解码；这里原先换成 SDK 自带的软件 libopus，实测每帧约 3 ms、
    // 在游戏混音线程上占半个核，发热降频后跟不上实时，背景音乐断续。改为宿主自带、NEON 优化的 libopus 1.5.2：
    // 虚表拷到宿主内存，Initialize / Reset / DecodeInterleaved 三槽包一层。输入每包 8 字节头（大端长度 + 终值），
    // 输出交错 PCM16；参数与返回值由真机探针确认（consumed = 8 + 长度，样本数 = 每声道帧数，返回 0）。
    // 宿主解码出错就这一下退回 SDK 原实现；files/opus-sdk 存在则整体用 SDK 解码。
    static void *vt_copy[10]; static int ready;
    void **vt = *(void ***)self;
    if (!ready) {
        for (int i = 0; i < 10; i++) vt_copy[i] = vt[i];
        sw_opus_orig = vt[9]; sw_opus_orig_init = vt[4]; sw_opus_orig_reset = vt[8];
        vt_copy[9] = (void *)sw_opus_wrap; vt_copy[4] = (void *)sw_opus_init_wrap; vt_copy[8] = (void *)sw_opus_reset_wrap; ready = 1;
        { const char *sv = getenv("SW_SAVE"); char m[1024]; if (sv) { snprintf(m, sizeof m, "%s/../opus-sdk", sv); sw_opus_use_host = access(m, F_OK) != 0; } }
        LOG("Opus 解码：%s", sw_opus_use_host ? "宿主 libopus 1.5.2（NEON）" : "SDK 软件解码（files/opus-sdk）");
    }
    g_vt_copy = vt_copy; if (vt != vt_copy) *(void ***)self = vt_copy;
}
void *sw_opus_orig, *sw_opus_orig_init, *sw_opus_orig_reset;
int sw_opus_use_host = 1;
_Atomic uint64_t sw_opus_ns, sw_opus_calls, sw_opus_fallback;
typedef uint64_t (*fwd8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static pthread_mutex_t opus_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t self; OpusDecoder *dec; int rate, ch; } opus_tab[64];
static OpusDecoder *opus_for(uint64_t self, int create) {   // 每个游戏解码器对象一个宿主解码器；采样率 / 声道数从对象自己的虚函数取
    int (*rate_fn)(uint64_t) = (int (*)(uint64_t))((void **)vt_copy_ptr())[6], (*ch_fn)(uint64_t) = (int (*)(uint64_t))((void **)vt_copy_ptr())[7];
    int rate = rate_fn(self), ch = ch_fn(self); if (ch < 1 || ch > 2 || (rate != 48000 && rate != 24000 && rate != 16000 && rate != 12000 && rate != 8000)) return NULL;
    int freeslot = -1;
    for (int i = 0; i < 64; i++) { if (opus_tab[i].self == self) { if (opus_tab[i].rate == rate && opus_tab[i].ch == ch && !create) return opus_tab[i].dec; opus_decoder_destroy(opus_tab[i].dec); opus_tab[i].self = 0; freeslot = i; break; } if (!opus_tab[i].self && freeslot < 0) freeslot = i; }
    if (freeslot < 0) return NULL;
    int err = 0; OpusDecoder *d = opus_decoder_create(rate, ch, &err); if (!d || err != OPUS_OK) return NULL;
    opus_tab[freeslot].self = self; opus_tab[freeslot].dec = d; opus_tab[freeslot].rate = rate; opus_tab[freeslot].ch = ch; return d;
}
uint64_t sw_opus_init_wrap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    uint64_t r = ((fwd8)sw_opus_orig_init)(a0, a1, a2, a3, a4, a5, a6, a7);
    if (sw_opus_use_host) { pthread_mutex_lock(&opus_lock); opus_for(a0, 1); pthread_mutex_unlock(&opus_lock); }
    return r;
}
uint64_t sw_opus_reset_wrap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    uint64_t r = ((fwd8)sw_opus_orig_reset)(a0, a1, a2, a3, a4, a5, a6, a7);
    if (sw_opus_use_host) { pthread_mutex_lock(&opus_lock); OpusDecoder *d = opus_for(a0, 0); if (d) opus_decoder_ctl(d, OPUS_RESET_STATE); pthread_mutex_unlock(&opus_lock); }
    return r;
}
// DecodeInterleaved(this, size_t *consumed, int *samples, int16_t *out, size_t out_bytes, const void *in, size_t in_bytes)
uint64_t sw_opus_wrap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t r = 1; int done = 0;
    if (sw_opus_use_host && a1 && a2 && a3 && a5 && a6 >= 8) {
        const uint8_t *in = (const uint8_t *)a5; uint32_t len = (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
        if ((uint64_t)len + 8 <= a6) {
            pthread_mutex_lock(&opus_lock); OpusDecoder *d = opus_for(a0, 0);
            int ch = 0; for (int i = 0; i < 64; i++) if (opus_tab[i].self == a0) ch = opus_tab[i].ch;
            int n = d && ch ? opus_decode(d, in + 8, (opus_int32)len, (opus_int16 *)a3, (int)(a4 / (2u * (unsigned)ch)), 0) : -1;
            pthread_mutex_unlock(&opus_lock);
            if (n >= 0) { *(uint64_t *)a1 = 8 + (uint64_t)len; *(int32_t *)a2 = n; r = 0; done = 1; }
        }
    }
    if (!done) { if (sw_opus_use_host) atomic_fetch_add(&sw_opus_fallback, 1); r = ((fwd8)sw_opus_orig)(a0, a1, a2, a3, a4, a5, a6, a7); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    atomic_fetch_add(&sw_opus_ns, (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t1.tv_nsec - t0.tv_nsec)); atomic_fetch_add(&sw_opus_calls, 1);
    return r;
}
