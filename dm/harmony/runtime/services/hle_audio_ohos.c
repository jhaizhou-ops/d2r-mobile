// nn::audio AudioOut：游戏（FMOD nx 输出层）按块追加 PCM 缓冲，播完由系统放回「已释放」队列并触发事件。
// 宿主：真出声 = CoreAudio AudioUnit（Mac DefaultOutput / iOS RemoteIO）；静音接收端 = 按真实时间消耗缓冲（不出声，节奏与真机一致）。
//       iOS 缺省真出声（产品必须有声音，总指挥 10-03），测试要静音显式 SW_AUDIO=0；Mac 缺省静音接收端（回放 / 闸门不出声），SW_AUDIO=1 真出声。
// AudioOut / AudioOutBuffer / AudioOutParameter 的内容只经下列接口读写，宿主自定内部布局，只写结构体前部。
#include "hle_ohos_compat.h"
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>

#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };

typedef struct { void *ptr; uint64_t cap, size; } gbuf;     // 写进客体 AudioOutBuffer 的前 24 字节
#define QMAX 64
#define RELMAX QMAX                    // 已释放最多 = 在途缓冲数（游戏收回缓冲才能再追加），与待播同上限
typedef struct {
    pthread_mutex_t m;
    void *event;                       // 客体 SystemEvent：有缓冲被释放就触发
    int ch;
    _Atomic int state;
    gbuf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // 待播：游戏侧写，出声端读
    gbuf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // 已释放：出声端写，游戏侧读
    uint64_t pos;                      // 当前块已播字节
    OH_AudioRenderer *au; int sink;
    pthread_t null_th; _Atomic int null_run;
    sem_t notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
    // 宿主环形缓冲（Claude）：喂入线程每 2 ms 把待播块搬进环并立刻释放给游戏，声卡回调只读环。
    pthread_mutex_t cm, rm; uint8_t *ring; size_t rcap, rhead, rfill, target; int primed; pthread_t feed_th; _Atomic int feed_run;
} aout;

// 打开中的输出登记表：前端生命周期（切后台）一次性暂停 / 恢复
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_suspended;
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p 不是宿主建的", g); return a; }

// 混音线程放大核（Claude）：客体线程默认 QoS，系统常把它排到小核，游戏混音跟不上实时（实测产出 64%）。
// 调 AppendAudioOutBuffer 的线程就是 FMOD 混音线程；连同宿主音频喂入/通知线程一起设 QOS_USER_INTERACTIVE。
#include <dlfcn.h>
static void audio_thread_boost(const char *who) {
    static __thread int done; if (done) return; done = 1;
    static int (*set_qos)(int); static int loaded;
    if (!loaded) { loaded = 1; void *h = dlopen("libqos.so", RTLD_NOW); if (h) set_qos = (int (*)(int))dlsym(h, "OH_QoS_SetThreadQoS"); }
    int r = set_qos ? set_qos(5 /* QOS_USER_INTERACTIVE */) : -1;
    LOG("音频线程提速：%s tid=%d QoS=USER_INTERACTIVE 结果 %d", who, (int)gettid(), r);
}
static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        if(sem_post(&a->notify_sem))die("audio sem_post");
}

static void *event_notify(void *p) {
    aout *a = p; audio_thread_boost("通知线程");
    for (;;) {
        while(sem_wait(&a->notify_sem))if(errno!=EINTR)die("audio sem_wait");
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        atomic_store_explicit(&a->notify_armed, 0, memory_order_release);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            if(sem_post(&a->notify_sem))die("audio sem_post");
    }
    return NULL;
}

static _Atomic uint64_t g_underrun_events, g_underrun_bytes;   // 欠载：出声端要数据时待播队列空（Claude 诊断）
static _Atomic uint64_t g_cb_max; uint64_t sw_audio_cb_max(void) { return atomic_exchange(&g_cb_max, 0); }
static _Atomic uint64_t g_app_bytes, g_app_calls, g_rel_ns, g_lat_ns, g_lat_n, g_lat_max, g_q_sum, g_q_n;
void sw_audio_stats(uint64_t *app_bytes, uint64_t *app_calls, uint64_t *lat_ns, uint64_t *lat_n, uint64_t *lat_max, uint64_t *q_sum, uint64_t *q_n) {
    *app_bytes = g_app_bytes; *app_calls = g_app_calls; *lat_ns = g_lat_ns; *lat_n = g_lat_n; *lat_max = atomic_exchange(&g_lat_max, 0); *q_sum = g_q_sum; *q_n = g_q_n; }
void sw_audio_underruns(uint64_t *events, uint64_t *bytes) { *events = atomic_load_explicit(&g_underrun_events, memory_order_relaxed); *bytes = atomic_load_explicit(&g_underrun_bytes, memory_order_relaxed); }
// 从待播队列取 n 字节交给出声端；整块播完就挪到已释放并通知普通线程。
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    int released = 0;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) { if (qh == qt && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) { atomic_fetch_add_explicit(&g_underrun_events, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_underrun_bytes, n, memory_order_relaxed); } memset(dst, 0, n); break; }
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
    if (released) { atomic_store_explicit(&g_rel_ns, now_ns(), memory_order_relaxed); notify_release(a); }
}

static _Atomic uint64_t g_render_calls;   // 声卡回调累计（生命周期验证：恢复后须继续增长）
uint64_t sw_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }
static OH_AudioData_Callback_Result au_render(OH_AudioRenderer *renderer,void *ctx,void *buffer,int32_t bytes) {
    (void)renderer;if(bytes<0)return AUDIO_DATA_CALLBACK_RESULT_INVALID;
    atomic_fetch_add_explicit(&g_render_calls,1,memory_order_relaxed);
    aout *a=ctx;uint8_t *dst=buffer;size_t n=(size_t)bytes,got=0;
    if(n>atomic_load_explicit(&g_cb_max,memory_order_relaxed))atomic_store_explicit(&g_cb_max,n,memory_order_relaxed);
    // Ring target follows the device callback size (>= 3 callbacks); a short ring is topped up from the queue here.
    pthread_mutex_lock(&a->rm);{size_t floor=(size_t)RATE*60/1000*2*a->ch;size_t want=3*n>floor?3*n:floor;if(want>a->rcap-n)want=a->rcap-n;if(a->target<want)a->target=want;}size_t fill=a->rfill;pthread_mutex_unlock(&a->rm);
    if(fill<n&&!pthread_mutex_trylock(&a->cm)){
        uint64_t queued=0;uint32_t qh=atomic_load_explicit(&a->q_head,memory_order_relaxed),qt=atomic_load_explicit(&a->q_tail,memory_order_acquire);
        for(uint32_t i=qh;i!=qt;i++)queued+=a->q[i&(QMAX-1)]->size;queued-=queued>=a->pos?a->pos:queued;
        static uint8_t direct[48000/5*2*8];size_t want=n-fill;if(want>queued)want=(size_t)queued;if(want>sizeof direct)want=sizeof direct;want-=want%(2u*a->ch);
        if(want){pull(a,direct,want);pthread_mutex_lock(&a->rm);size_t tail=(a->rhead+a->rfill)%a->rcap,first=a->rcap-tail<want?a->rcap-tail:want;memcpy(a->ring+tail,direct,first);memcpy(a->ring,direct+first,want-first);a->rfill+=want;a->primed=1;pthread_mutex_unlock(&a->rm);}
        pthread_mutex_unlock(&a->cm);}
    pthread_mutex_lock(&a->rm);
    if(!a->primed&&a->rfill>=a->target/2)a->primed=1;
    if(a->primed){got=a->rfill<n?a->rfill:n;size_t first=a->rcap-a->rhead<got?a->rcap-a->rhead:got;memcpy(dst,a->ring+a->rhead,first);memcpy(dst+first,a->ring,got-first);a->rhead=(a->rhead+got)%a->rcap;a->rfill-=got;}
    if(got<n&&atomic_load_explicit(&a->state,memory_order_acquire)==STATE_STARTED){atomic_fetch_add_explicit(&g_underrun_events,1,memory_order_relaxed);atomic_fetch_add_explicit(&g_underrun_bytes,(n-got)*2/a->ch,memory_order_relaxed);a->primed=0;}
    pthread_mutex_unlock(&a->rm);
    if(got<n)memset(dst+got,0,n-got);
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}
static void *ring_feed(void *p) {
    aout *a=p;static uint8_t scratch[48000/10*2*8];audio_thread_boost("喂入线程");
    while(atomic_load_explicit(&a->feed_run,memory_order_acquire)){
        usleep(2000);
        if(atomic_load_explicit(&a->state,memory_order_acquire)!=STATE_STARTED||a->sink!=1){pthread_mutex_lock(&a->rm);a->rfill=0;a->rhead=0;a->primed=0;pthread_mutex_unlock(&a->rm);continue;}
        pthread_mutex_lock(&a->cm);
        uint64_t queued=0;uint32_t qh=atomic_load_explicit(&a->q_head,memory_order_relaxed),qt=atomic_load_explicit(&a->q_tail,memory_order_acquire);
        for(uint32_t i=qh;i!=qt;i++)queued+=a->q[i&(QMAX-1)]->size;queued-=queued>=a->pos?a->pos:queued;
        pthread_mutex_lock(&a->rm);size_t room=a->rfill<a->target?a->target-a->rfill:0;pthread_mutex_unlock(&a->rm);
        size_t n=queued<room?(size_t)queued:room;if(n>sizeof scratch)n=sizeof scratch;n-=n%(2u*a->ch);
        if(n){pull(a,scratch,n);pthread_mutex_lock(&a->rm);size_t tail=(a->rhead+a->rfill)%a->rcap,first=a->rcap-tail<n?a->rcap-tail:n;memcpy(a->ring+tail,scratch,first);memcpy(a->ring,scratch+first,n-first);a->rfill+=n;pthread_mutex_unlock(&a->rm);}
        pthread_mutex_unlock(&a->cm);
    }
    return NULL;
}
static void au_open(aout *a) {
    OH_AudioStreamBuilder *b=NULL;OH_AudioStream_Result r=OH_AudioStreamBuilder_Create(&b,AUDIOSTREAM_TYPE_RENDERER);
    if(!r)r=OH_AudioStreamBuilder_SetSamplingRate(b,RATE);
    if(!r)r=OH_AudioStreamBuilder_SetChannelCount(b,a->ch);
    if(!r)r=OH_AudioStreamBuilder_SetSampleFormat(b,AUDIOSTREAM_SAMPLE_S16LE);
    if(!r)r=OH_AudioStreamBuilder_SetEncodingType(b,AUDIOSTREAM_ENCODING_TYPE_RAW);
    if(!r)r=OH_AudioStreamBuilder_SetRendererInfo(b,AUDIOSTREAM_USAGE_GAME);
    if(!r){OH_AudioStream_Result lm=OH_AudioStreamBuilder_SetLatencyMode(b,AUDIOSTREAM_LATENCY_MODE_FAST);LOG("音频：低延迟模式 FAST %s（%d）",lm?"失败":"已开",lm);}
    if(!r){OH_AudioStream_Result fr=OH_AudioStreamBuilder_SetFrameSizeInCallback(b,RATE/50);if(fr)LOG("音频：设置回调帧长 20ms 失败 %d，用系统缺省",fr);}   // 低延迟：每次回调 20ms
    if(!r)r=OH_AudioStreamBuilder_SetRendererWriteDataCallback(b,au_render,a);
    if(!r)r=OH_AudioStreamBuilder_GenerateRenderer(b,&a->au);
    if(b)OH_AudioStreamBuilder_Destroy(b);
    if(r)die("OHAudio renderer failed: %d channels=%d",r,a->ch);
    a->rcap=(size_t)RATE*2/5*2*a->ch;a->target=(size_t)RATE*60/1000*2*a->ch;a->ring=calloc(1,a->rcap);if(!a->ring)die("audio ring");
    atomic_init(&a->feed_run,1);
    if(pthread_create(&a->feed_th,NULL,ring_feed,a))die("audio ring feeder");
}
// 静音接收端：每 5 ms 按 48 kHz 实时消耗
static void *null_sink(void *p) {
    aout *a = p; uint8_t scratch[RATE / 200 * 2 * 8];
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    while (atomic_load_explicit(&a->null_run, memory_order_acquire)) {
        t += 5000000; uint64_t now = now_ns(); if (t > now) usleep((useconds_t)((t - now) / 1000));
        pthread_mutex_lock(&a->cm); pull(a, scratch, bytes); pthread_mutex_unlock(&a->cm);
    }
    return NULL;
}
static int real_audio(void) { const char *e = getenv("SW_AUDIO"); return e ? atoi(e) != 0 : 1; }

#include "audio_ohos_lifecycle.inc"

// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }，0 = 系统默认（48 kHz / 2 声道）
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
    if(ch<1||ch>8)return RES_FAIL;
    if (rate && rate != RATE) die("AudioOut 采样率 %d 不支持（只做 48 kHz）", rate);
    aout *a = calloc(1, sizeof *a);if(!a)return RES_FAIL;
    pthread_mutex_init(&a->m, NULL); a->event = ev; a->ch = ch;
    atomic_init(&a->state, STATE_STOPPED);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    if(sem_init(&a->notify_sem,0,0))die("audio sem_init");
    pthread_mutex_init(&a->cm,NULL);pthread_mutex_init(&a->rm,NULL);
    if(pthread_create(&a->notify_th, NULL, event_notify, a))die("audio notifier thread");
    if (ev) hle_bind_system_event(ev, 1);   // 缓冲释放事件：客体线程上绑内核事件（自动清除），之后由音频线程触发
    if (real_audio()) au_open(a);
    objmap_put(g, a);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("AudioOut 同时打开超过 %d 个", AOMAX);
    g_aos[slot] = a;
    pthread_mutex_unlock(&g_aos_lock);
    LOG("audio::OpenAudioOut %s 声道 %d %s", name ? name : "(默认)", ch, real_audio() ? "真出声" : "静音接收端");

    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "开始播放", Result, h_au_Start, void *g) {
    return xblr_audio_start(ao(g));
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "停止播放", void, h_au_Stop, void *g) {
    if(xblr_audio_stop(ao(g)))die("OHAudio stop failed; consumer was not replaced");
}
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "关闭音频输出", void, h_au_Close, void *g) {
    h_au_Stop(g);
    aout *a = objmap_del(g);
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    if(sem_post(&a->notify_sem))die("audio sem_post");
    pthread_join(a->notify_th, NULL);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    pthread_mutex_unlock(&g_aos_lock);
    if (a->au) { atomic_store_explicit(&a->feed_run,0,memory_order_release); pthread_join(a->feed_th,NULL); free(a->ring); }
    if (a->au) { if(OH_AudioRenderer_Release(a->au))die("OHAudio release failed"); }
    sem_destroy(&a->notify_sem);
    pthread_mutex_destroy(&a->m); free(a);
}
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "播放状态", int, h_au_State, const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "采样率", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "声道数", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "采样格式；只做 PCM16", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }

HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "缓冲描述；写进客体结构体前 24 字节", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) {
    if(!p||size>cap)die("invalid audio buffer");b->ptr = p; b->cap = cap; b->size = size;
}
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "缓冲数据指针", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "缓冲数据大小", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "追加待播缓冲", unsigned, h_au_Append, void *g, gbuf *b) {
    aout *a = ao(g);if(!b||!b->ptr||!b->size||b->size>b->cap||b->size%(2u*a->ch))return 0;
    audio_thread_boost("游戏混音线程");
    // 诊断开关 SW_AUDIO_DUMP=<文件>：游戏追加的每块 PCM 原样续写（16 位、48 kHz、声道数见 OpenAudioOut 日志，常见 6）。判「某个音播没播」用：
    // 拿原版音频对它做互相关（功耗线 10-06 一档掉落音就这么验的）。只在设了变量时打开文件，没设时这里只多一次已缓存的判断
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("SW_AUDIO_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("【红】SW_AUDIO_DUMP 打不开 %s", e); }
      if (df && b && b->ptr && b->size) fwrite(b->ptr, 1, (size_t)b->size, df); }
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    int ok = qt - qh < QMAX;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release);
        atomic_fetch_add(&g_app_bytes, b->size * 2 / a->ch); atomic_fetch_add(&g_app_calls, 1); atomic_fetch_add(&g_q_sum, qt + 1 - qh); atomic_fetch_add(&g_q_n, 1);
        uint64_t r = atomic_exchange(&g_rel_ns, 0); if (r) { uint64_t d = now_ns() - r; atomic_fetch_add(&g_lat_ns, d); atomic_fetch_add(&g_lat_n, 1); if (d > g_lat_max) atomic_store(&g_lat_max, d); } }
    pthread_mutex_unlock(&a->m);
    return ok;      // bool：队列满时 false（真机同样有上限）
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
int sw_audio_suspend(int on) { return xblr_audio_suspend_all(on); }
