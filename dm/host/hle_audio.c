// nn::audio AudioOut：游戏（FMOD nx 输出层）按块追加 PCM 缓冲，播完由系统放回「已释放」队列并触发事件。
// 宿主：真出声 = CoreAudio AudioUnit（Mac DefaultOutput / iOS RemoteIO）；静音接收端 = 按真实时间消耗缓冲（不出声，节奏与真机一致）。
//       iOS 缺省真出声（产品必须有声音），测试要静音显式 DM_AUDIO=0；Mac 命令行宿主缺省静音接收端（回放 / 闸门不出声），DM_AUDIO=1 真出声；
//       Mac App（app_mac.m）是产品，启动时设 DM_AUDIO=1（「没有声音」：之前 App 也走了命令行的缺省，从来不出声）。
//       声卡打不开（没有输出设备等）不再退出：记红，这一路改由静音接收端按实时消耗，游戏照常跑。
// AudioOut / AudioOutBuffer / AudioOutParameter 的内容只经下列接口读写，宿主自定内部布局，只写结构体前部。
// nn::audio AudioOut: the game (FMOD's nx output layer) appends PCM buffers in blocks; once played the system puts them back on the "released" queue and signals an event.
// Host: real sound = CoreAudio AudioUnit (Mac DefaultOutput / iOS RemoteIO); silent sink = buffers consumed in real time (no sound, same pacing as the device).
//       iOS plays real sound by default (the product must have sound), tests set DM_AUDIO=0 explicitly for silence; the Mac command-line host defaults to the silent sink (replays / gates are silent), DM_AUDIO=1 for real sound;
//       the Mac App (app_mac.m) is the product and sets DM_AUDIO=1 at launch ("no sound": the App used to take the command-line default and never played sound).
//       If the sound card cannot be opened (no output device etc.) it no longer exits: logs red, and this channel is consumed in real time by the silent sink while the game runs on.
// The contents of AudioOut / AudioOutBuffer / AudioOutParameter are read and written only through the interfaces below; the host defines its own internal layout and writes only the front part of each struct.
#include "hle.h"
#ifdef __ANDROID__
#include "../android/audio_aaudio.inc"
#else
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };

typedef struct { void *ptr; uint64_t cap, size; } gbuf;     // 写进客体 AudioOutBuffer 的前 24 字节 | The first 24 bytes written into the guest AudioOutBuffer
#define QMAX 64
#define RELMAX QMAX                    // 已释放最多 = 在途缓冲数（游戏收回缓冲才能再追加），与待播同上限 | Released max = number of in-flight buffers (the game must take a buffer back before it can append again), same limit as pending
typedef struct {
    pthread_mutex_t m;
    void *event;                       // 客体 SystemEvent：有缓冲被释放就触发 | Guest SystemEvent: signaled whenever a buffer is released
    int ch;
    _Atomic int state;
    gbuf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // 待播：游戏侧写，出声端读 | Pending: written by the game side, read by the output side
    gbuf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // 已释放：出声端写，游戏侧读 | Released: written by the output side, read by the game side
    uint64_t pos;                      // 当前块已播字节 | Bytes already played of the current block
    AudioUnit au;
    int mix;                           // 1 = 游戏给 5.1（6 声道），交给 CoreAudio 前我们自己下混成双声道（au_render） | 1 = the game gives 5.1 (6 channels), downmixed to stereo by us before handing to CoreAudio (au_render)
    int16_t *mixbuf;                   // 下混用的 6 声道暂存（MIXMAX 帧） | 6-channel staging for the downmix (MIXMAX frames)
    pthread_t null_th; _Atomic int null_run;
    dispatch_semaphore_t notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
    _Atomic uint64_t notify_ns;        // 最近一次「有缓冲释放、要通知」的时刻（量通知线程被叫醒的延迟） | Time of the most recent "buffer released, needs notifying" event (to measure the notification thread's wake-up latency)
    int dry;                           // 这次声卡回调里待播队列空了（在播放态）= 一次欠载（断音） | The pending playback queue was empty in this audio callback (while playing) = one underrun (audio dropout)
} aout;

// 打开中的输出登记表：前端生命周期（切后台）一次性暂停 / 恢复
// Registry of open outputs: suspended / resumed all at once by the front-end lifecycle (going to background)
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_suspended;
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p 不是宿主建的", g); return a; }

// 通知线程 QoS 对照（公开 iPhone 15 Pro Max 移动时断音，提议把这条线程提到交互级、与混音线程一致）。
// 缺省交互级（真机 iPhone18,2 野外移动 240 秒同进程交替：叫醒延迟平均约减半、超过 2 ms 的 16 → 0，7 段配对 7/7；欠载两相都是 0，没有可测的退步）。
// DM_AUDIO_NQOS：不设 / 1 = 交互级；0 = 旧行为（继承建线程者的 QoS）；alt:<秒> = 同一进程里每 <秒> 交替一次，每段结束记一行这段的欠载次数与通知延迟——
// 真机移动时同进程交替比，不用两次启动（热态、场景都不同）对比
// Notification thread QoS comparison (public report of audio dropouts while moving on iPhone 15 Pro Max; proposal: raise this thread to interactive level, same as the mixer thread).
// Default is interactive (device iPhone18,2, 240 s of wilderness movement with in-process alternation: wake-up latency roughly halved on average, cases over 2 ms 16 → 0, 7/7 paired segments; underruns 0 in both phases, no measurable regression).
// DM_AUDIO_NQOS: unset / 1 = interactive; 0 = old behavior (inherits the QoS of the thread's creator); alt:<seconds> = alternate every <seconds> within the same process, logging at the end of each segment its underrun count and notification latency;
// compare by in-process alternation while moving on the device, not across two launches (thermal state and scene both differ)
static int g_nq_mode = -1, g_nq_alt; static qos_class_t g_nq_base = QOS_CLASS_UNSPECIFIED;
static _Atomic uint64_t g_nq_dry[2], g_nq_calls[2], g_nq_wake[2], g_nq_lat_sum[2], g_nq_lat_max[2], g_nq_lat_2ms[2];
static int nq_phase(void) { if (g_nq_mode < 0) { const char *e = getenv("DM_AUDIO_NQOS"); g_nq_mode = e && !strcmp(e, "0") ? 0 : e && !strncmp(e, "alt:", 4) ? 2 : 1; if (g_nq_mode == 2) { g_nq_alt = atoi(e + 4); if (g_nq_alt < 1) g_nq_alt = 30; } }
    return g_nq_mode < 2 ? g_nq_mode : (int)((now_ns() / 1000000000ull / (uint64_t)g_nq_alt) & 1); }
static void nq_report(int ph) {
    uint64_t c = atomic_exchange(&g_nq_calls[ph], 0), d = atomic_exchange(&g_nq_dry[ph], 0), w = atomic_exchange(&g_nq_wake[ph], 0), ls = atomic_exchange(&g_nq_lat_sum[ph], 0), lm = atomic_exchange(&g_nq_lat_max[ph], 0), l2 = atomic_exchange(&g_nq_lat_2ms[ph], 0);
    if (!c) return;
    if (ph) LOG("音频通知线程 QoS 对照：相 交互级：声卡回调 %llu 次、欠载（断音）%llu 次；通知 %llu 次，叫醒延迟 平均 %.3f ms、最大 %.3f ms、超过 2 ms %llu 次", (unsigned long long)c, (unsigned long long)d, (unsigned long long)w, w ? ls / 1e6 / (double)w : 0.0, lm / 1e6, (unsigned long long)l2);
    else LOG("音频通知线程 QoS 对照：相 现状：声卡回调 %llu 次、欠载（断音）%llu 次；通知 %llu 次，叫醒延迟 平均 %.3f ms、最大 %.3f ms、超过 2 ms %llu 次", (unsigned long long)c, (unsigned long long)d, (unsigned long long)w, w ? ls / 1e6 / (double)w : 0.0, lm / 1e6, (unsigned long long)l2);
}
static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_ns, now_ns(), memory_order_relaxed);
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        dispatch_semaphore_signal(a->notify_sem);
}

static void *event_notify(void *p) {
    aout *a = p; int applied = -1;
    if (g_nq_base == QOS_CLASS_UNSPECIFIED) g_nq_base = qos_class_self();
    for (;;) {
        int ph = nq_phase();
        if (ph != applied) {   // 换相：先报上一相这段的读数，再改本线程 QoS | Phase switch: first report the readings of the previous phase's segment, then change this thread's QoS
            if (applied >= 0) nq_report(applied);
            int e = pthread_set_qos_class_self_np(ph ? QOS_CLASS_USER_INTERACTIVE : (g_nq_base == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : g_nq_base), 0);
            if (e) LOG("【红】音频通知线程：设 QoS 失败（%d）", e);
            else if (g_nq_mode && ph) LOG("音频通知线程：QoS → 交互级"); else if (g_nq_mode) LOG("音频通知线程：QoS → 现状");
            applied = ph; }
        dispatch_semaphore_wait(a->notify_sem, DISPATCH_TIME_FOREVER);
        { uint64_t t0 = atomic_load_explicit(&a->notify_ns, memory_order_relaxed), dt = t0 ? now_ns() - t0 : 0; int q = applied < 0 ? 0 : applied;
          atomic_fetch_add(&g_nq_wake[q], 1); atomic_fetch_add(&g_nq_lat_sum[q], dt); if (dt > 2000000) atomic_fetch_add(&g_nq_lat_2ms[q], 1);
          uint64_t mx = atomic_load(&g_nq_lat_max[q]); while (dt > mx && !atomic_compare_exchange_weak(&g_nq_lat_max[q], &mx, dt)) {} }
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        atomic_store_explicit(&a->notify_armed, 0, memory_order_release);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            dispatch_semaphore_signal(a->notify_sem);
    }
    return NULL;
}

// 从待播队列取 n 字节交给出声端；整块播完就挪到已释放并通知普通线程。
// Take n bytes from the pending queue and hand them to the output side; once a whole block has played, move it to released and notify the ordinary thread.
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    int released = 0;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) { if (qh == qt && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) a->dry = 1; memset(dst, 0, n); break; }
        gbuf *b = a->q[qh & (QMAX - 1)];
        uint64_t k = b->size - a->pos; if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, k);
        dst += k; n -= k; a->pos += k;
        if (a->pos >= b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) die("AudioOut 已释放队列溢出");
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            qh++;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released = 1;
        }
    }
    if (released) notify_release(a);
}

static _Atomic uint64_t g_render_calls;   // 声卡回调累计（生命周期验证：恢复后须继续增长） | Cumulative sound card callbacks (lifecycle verification: must keep growing after resume)
uint64_t dm_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }
// 5.1 → 双声道（公开 的：原先把游戏的 6 声道原样交给 AudioUnit、不设声道布局，系统隐式转换只取前两路，中置——NPC 对白——整路丢掉）。
// 游戏的声道顺序（Mac App 走到阿卡拉跟前、对白开口那半秒按声道量能量实测：只有第 3 路跳起来）：左前、右前、中置、低音、左后、右后。
// 系数照 ITU-R BS.775 的 Lo/Ro：L = 左前 + 0.707·中置 + 0.707·左后，R 对称；低音不并入（标准下混不含 LFE，并进去会让低频发闷）；
// 整体再乘 0.707 留 3 dB 余量，仍超出 16 位的饱和截断。Q15 定点：0.707 = 23170，0.707 × 0.707 = 16384
// 5.1 -> stereo (previously: the game's 6 channels were handed to the AudioUnit as is without a channel layout; the system's implicit conversion took only the first two, and the centre - NPC dialogue - was dropped entirely).
// The game's channel order (measured in the Mac App by per-channel energy during the half second Akara starts speaking: only channel 3 jumps): front left, front right, centre, LFE, rear left, rear right.
// Coefficients from the ITU-R BS.775 Lo/Ro downmix: L = front left + 0.707*centre + 0.707*rear left, R symmetric; LFE is not mixed in (the standard downmix excludes LFE, and mixing it in muddies the low end);
// the whole is multiplied by 0.707 again for 3 dB of headroom, and anything still beyond 16 bits is saturated. Q15 fixed point: 0.707 = 23170, 0.707 x 0.707 = 16384
#define MIXMAX 4096
static void downmix51(const int16_t *in, int16_t *out, unsigned frames) {
    for (unsigned i = 0; i < frames; i++, in += 6, out += 2) {
        int32_t l = (23170 * in[0] + 16384 * in[2] + 16384 * in[4]) >> 15, r = (23170 * in[1] + 16384 * in[2] + 16384 * in[5]) >> 15;
        out[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l); out[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r); }
}
static OSStatus au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)f; (void)ts; (void)bus; (void)frames;
    aout *a = ctx;
    atomic_fetch_add_explicit(&g_render_calls, 1, memory_order_relaxed);
    a->dry = 0;
    if (!a->mix) pull(a, bl->mBuffers[0].mData, bl->mBuffers[0].mDataByteSize);
    else { int16_t *o = bl->mBuffers[0].mData; unsigned left = bl->mBuffers[0].mDataByteSize / 4;   // 双声道 16 位：每帧 4 字节 | stereo 16-bit: 4 bytes per frame
        while (left) { unsigned k = left < MIXMAX ? left : MIXMAX; pull(a, (uint8_t *)a->mixbuf, (uint64_t)k * 12); downmix51(a->mixbuf, o, k); o += 2 * k; left -= k; } }
    // 诊断开关 DM_AUDIO_OUT_DUMP=<文件>：交给 CoreAudio 的那份数据原样续写（下混后是双声道）。本机没有回环设备录不到系统输出，判「对白有没有交出去」看这份
    // Diagnostic switch DM_AUDIO_OUT_DUMP=<file>: append the data handed to CoreAudio as is (stereo after the downmix). This machine has no loopback device and cannot record the system output; to tell whether the dialogue was handed over, look at this file
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDIO_OUT_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("【红】DM_AUDIO_OUT_DUMP 打不开 %s", e); }
      if (df) fwrite(bl->mBuffers[0].mData, 1, bl->mBuffers[0].mDataByteSize, df); }
    { int q = nq_phase(); atomic_fetch_add_explicit(&g_nq_calls[q], 1, memory_order_relaxed); if (a->dry) atomic_fetch_add_explicit(&g_nq_dry[q], 1, memory_order_relaxed); }   // 通知线程 QoS 对照：按当前相记 | Notification thread QoS comparison: recorded per current phase
    return noErr;
}
#if !TARGET_OS_IPHONE
static void dev_name(AudioObjectID id, char *o, size_t n) {   // 输出设备名（日志用） | output device name (for the log)
    CFStringRef nm = NULL; UInt32 sz = sizeof nm; AudioObjectPropertyAddress pa = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    o[0] = 0; if (!AudioObjectGetPropertyData(id, &pa, 0, NULL, &sz, &nm) && nm) { CFStringGetCString(nm, o, (CFIndex)n, kCFStringEncodingUTF8); CFRelease(nm); } }
#endif
static int au_open(aout *a) {   // 0 = 声卡开好了；-1 = 开不了（a->au 置空，调用方走静音接收端） | 0 = the sound card is open; -1 = it cannot be opened (a->au set to null, the caller uses the silent sink)
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
                                   .componentSubType = kAudioUnitSubType_RemoteIO,
#else
                                   .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
                                   .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &a->au)) { a->au = NULL; LOG("【红】AudioUnit 建不出来：这一路改用静音接收端（不出声）"); return -1; }
    a->mix = a->ch == 6; if (a->mix && !a->mixbuf) a->mixbuf = malloc(MIXMAX * 12);   // 5.1：我们自己下混，AudioUnit 只收双声道（单 / 双声道照旧原样交） | 5.1: we downmix ourselves and the AudioUnit only takes stereo (mono / stereo are passed through as before)
    if (a->mix && !a->mixbuf) { AudioComponentInstanceDispose(a->au); a->au = NULL; LOG("【红】下混暂存申请不到：这一路改用静音接收端（不出声）"); return -1; }
    int och = a->mix ? 2 : a->ch;
    AudioStreamBasicDescription f = {.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
                                     .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 2u * och, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * och,
                                     .mChannelsPerFrame = (UInt32)och, .mBitsPerChannel = 16};
    if (a->mix) LOG("audio：游戏给 6 声道（5.1）→ 交给声卡前自己下混成双声道（中置 / 后环绕 ×0.707 并入左右，低音不并入，整体 −3 dB、饱和截断）");
    AURenderCallbackStruct cb = {au_render, a};
    if (AudioUnitSetProperty(a->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(a->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(a->au)) { AudioComponentInstanceDispose(a->au); a->au = NULL; LOG("【红】AudioUnit 设置失败（没有输出设备？）：这一路改用静音接收端（不出声）"); return -1; }
#if !TARGET_OS_IPHONE
    {   // 缺省输出单元跟随系统缺省输出设备：两个都记下来，核「声音从系统选的那个设备出」 | the default output unit follows the system default output device: record both, to check that "the sound comes out of the device the system chose"
        AudioObjectID cur = 0, def = 0; UInt32 sz = sizeof cur; char cn[128], dn[128];
        AudioUnitGetProperty(a->au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &cur, &sz);
        AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }; sz = sizeof def;
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, &def); dev_name(cur, cn, sizeof cn); dev_name(def, dn, sizeof dn);
        LOG("audio：输出设备「%s」（%u），系统缺省输出「%s」（%u）%s", cn, (unsigned)cur, dn, (unsigned)def, cur == def ? "，一致" : "【红】不一致"); }
#endif
    return 0;
}
// 静音接收端：每 5 ms 按 48 kHz 实时消耗
// Silent sink: consumes in real time at 48 kHz every 5 ms
static void *null_sink(void *p) {
    aout *a = p; uint8_t scratch[RATE / 200 * 2 * 8];
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    while (atomic_load_explicit(&a->null_run, memory_order_acquire)) {
        t += 5000000; uint64_t now = now_ns(); if (t > now) usleep((useconds_t)((t - now) / 1000));
        pull(a, scratch, bytes);
    }
    return NULL;
}
static int real_audio(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : TARGET_OS_IPHONE; }

// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }，0 = 系统默认（48 kHz / 2 声道）
// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }, 0 = system default (48 kHz / 2 channels)
HLE_FORCE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", "音频输出参数默认值", void, h_au_InitParam, uint8_t *p) { memset(p, 0, 8); }
// AudioOutInfo = { char name[256] }
HLE_FORCE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "音频输出设备列表；宿主 1 个", int, h_au_List, char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256); strcpy(info, "DeviceOut"); return 1;
}
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", "连 audout 服务；宿主 AudioUnit / 静音接收端",
          Result, h_au_Open, void *g, void *ev, const char *name, const uint8_t *param) {
    int ch = *(const uint16_t *)(param + 4); int rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if (rate && rate != RATE) die("AudioOut 采样率 %d 不支持（只做 48 kHz）", rate);
    aout *a = calloc(1, sizeof *a);
    pthread_mutex_init(&a->m, NULL); a->event = ev; a->ch = ch;
    atomic_init(&a->state, STATE_STOPPED);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    a->notify_sem = dispatch_semaphore_create(0);
    pthread_create(&a->notify_th, NULL, event_notify, a);
    if (ev) hle_bind_system_event(ev, 1);   // 缓冲释放事件：客体线程上绑内核事件（自动清除），之后由音频线程触发 | Buffer release event: bind a kernel event on the guest thread (auto-clear), afterwards signaled by the audio thread
    objmap_put(g, a);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("AudioOut 同时打开超过 %d 个", AOMAX);
    g_aos[slot] = a;
    pthread_mutex_unlock(&g_aos_lock);
    LOG("audio::OpenAudioOut %s 声道 %d %s", name ? name : "(默认)", ch, real_audio() ? "真出声" : "静音接收端");
    if (real_audio()) au_open(a);   // 开不了就 a->au 为空，开始播放时走静音接收端 | if it cannot be opened, a->au is null, and playback goes to the silent sink when it starts
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "开始播放", Result, h_au_Start, void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STARTED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (was == STATE_STARTED) return RES_OK;
    if (a->au && !g_suspended && AudioOutputUnitStart(a->au)) { LOG("【红】AudioUnit 启动失败：这一路改用静音接收端（不出声）"); AudioUnitUninitialize(a->au); AudioComponentInstanceDispose(a->au); a->au = NULL; }
    if (!a->au || g_suspended) { atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); }
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "停止播放", void, h_au_Stop, void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STOPPED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (was != STATE_STARTED) return;
    pthread_mutex_lock(&g_aos_lock); int susp = g_suspended; pthread_mutex_unlock(&g_aos_lock);
    if (a->au && !susp) AudioOutputUnitStop(a->au);
    else { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); }   // 静音接收端（含暂停期间接手的） | Silent sink (including one that took over during suspension)
}
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "关闭音频输出", void, h_au_Close, void *g) {
    h_au_Stop(g);
    aout *a = objmap_del(g);
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    dispatch_semaphore_signal(a->notify_sem);
    pthread_join(a->notify_th, NULL);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    pthread_mutex_unlock(&g_aos_lock);
    if (a->au) { AudioUnitUninitialize(a->au); AudioComponentInstanceDispose(a->au); }
    dispatch_release(a->notify_sem);
    free(a->mixbuf); pthread_mutex_destroy(&a->m); free(a);
}
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "播放状态", int, h_au_State, const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "采样率", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "声道数", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "采样格式；只做 PCM16", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }

HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "缓冲描述；写进客体结构体前 24 字节", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) {
    b->ptr = p; b->cap = cap; b->size = size;
}
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "缓冲数据指针", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "缓冲数据大小", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "追加待播缓冲", unsigned, h_au_Append, void *g, gbuf *b) {
    aout *a = ao(g);
    // 诊断开关 DM_AUDIO_DUMP=<文件>：游戏追加的每块 PCM 原样续写（16 位、48 kHz、声道数见 OpenAudioOut 日志，常见 6）。判「某个音播没播」用：
    // 拿原版音频对它做互相关（一档掉落音就这么验的）。只在设了变量时打开文件，没设时这里只多一次已缓存的判断
    // Diagnostic switch DM_AUDIO_DUMP=<file>: every PCM block the game appends is written out as is (16-bit, 48 kHz, channel count per the OpenAudioOut log, usually 6). Used to judge "did a given sound play or not":
    // cross-correlate the original game's audio against it (this is how the tier-one drop sound was verified). The file is opened only when the variable is set; when unset this costs only one extra cached check
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDIO_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("【红】DM_AUDIO_DUMP 打不开 %s", e); }
      if (df && b && b->ptr && b->size) fwrite(b->ptr, 1, (size_t)b->size, df); }
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    int ok = qt - qh < QMAX;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return ok;      // bool：队列满时 false（真机同样有上限） | bool: false when the queue is full (the real device has a limit too)
}
HLE_FORCE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "取已播完的缓冲；没有则 NULL", gbuf *, h_au_Released, void *g) {
    aout *a = ao(g); gbuf *b = NULL;
    pthread_mutex_lock(&a->m);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rr != rw) { b = a->rel[rr & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rr + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return b;
}

// 前端生命周期 / 音频会话打断：暂停真出声，期间由静音接收端接手按实时消耗（节奏不断）；恢复时切回 AudioUnit。
// 前端在 切后台 / 打断开始 时调 1，回前台 / 打断结束（shouldResume）时调 0。
// Front-end lifecycle / audio session interruption: suspend real audio output, with the silent sink taking over real-time consumption meanwhile (pacing unbroken); switch back to the AudioUnit on resume.
// The front end calls with 1 on going to background / interruption began, and with 0 on returning to foreground / interruption ended (shouldResume).
void dm_audio_suspend(int on) {
    pthread_mutex_lock(&g_aos_lock);
    if (g_suspended != on) {
        g_suspended = on; int failed = 0;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i];
            if (!a || !a->au || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) continue;
            // 暂停期间改由静音接收端按实时消耗：否则缓冲不再释放，FMOD 输出线程永久等待、游戏音频节奏卡死（来电打断同理）
            // While suspended the silent sink consumes in real time instead: otherwise buffers are no longer released, the FMOD output thread waits forever and the game's audio pacing stalls (same for an incoming call interruption)
            if (on) { AudioOutputUnitStop(a->au); atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); }
            else { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL);
                   OSStatus st = AudioOutputUnitStart(a->au);
                   if (st) {   // 会话还被别人占着（通话没挂就切回游戏、打断结束时人还在别的 App）：不致命，继续由静音接收端顶着，等下一次恢复再试 | The session is still held by someone else (switched back to the game with a call not yet hung up, or the user is still in another app when the interruption ends): not fatal, the silent sink keeps covering, and we retry at the next resume
                       LOG("【红】AudioUnit 恢复失败（状态 %d）：保持静音接收端，等下次恢复重试", (int)st);
                       atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); failed = 1; } }
        }
        if (failed) g_suspended = 1;   // 没恢复成：状态留在「暂停」，下次调 0 会再试 | Resume did not succeed: the state stays "suspended", and the next call with 0 will retry
        LOG("audio %s", on ? "暂停（切后台 / 被打断）" : failed ? "恢复未成" : "恢复（回前台 / 打断结束）");
    }
    pthread_mutex_unlock(&g_aos_lock);
}
#endif // __ANDROID__: Apple implementation above stays unchanged.
