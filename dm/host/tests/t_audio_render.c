// 真实 au_render 回调测试：六声道转双声道、跨输入缓冲与 256 帧批次、收回顺序、欠载补零。
// Exercise the real au_render callback: six-to-two conversion across input buffers and 256-frame chunks, release ordering, and underrun silence.
#include "../hle_audio.c"
#include <time.h>
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *k; void *v; } om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; return; } }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) { om[i].k = 0; return om[i].v; } return NULL; }
static _Atomic int g_sig;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add_explicit(&g_sig, 1, memory_order_seq_cst); }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }   // 真实现要客体线程 + SDK 内核事件；单测只数触发次数 | the real implementation needs a guest thread + SDK kernel event; the unit test only counts triggers
#include <assert.h>
int main(void) {
    aout a = {0}; a.ch = 6;
    atomic_init(&a.state, STATE_STARTED);
    a.notify_sem = dispatch_semaphore_create(0);
    int16_t input[513 * 6] = {0};
    for (int i = 0; i < 513; i++) input[i * 6 + 2] = 10000;
    gbuf b[3] = {{input, 100*12, 100*12}, {input+100*6, 200*12, 200*12}, {input+300*6, 213*12, 213*12}};
    for (int i=0;i<3;i++) a.q[i]=&b[i];
    atomic_store(&a.q_tail, 3);
    struct { int16_t before; int16_t pcm[600*2]; int16_t after; } out;
    out.before=1234; out.after=5678; memset(out.pcm, 0x55, sizeof out.pcm);
    AudioBufferList bl={.mNumberBuffers=1,.mBuffers={{.mNumberChannels=2,.mDataByteSize=sizeof out.pcm,.mData=out.pcm}}};
    assert(au_render(&a,NULL,NULL,0,600,&bl)==noErr);
    for(int i=0;i<513*2;i++) assert(out.pcm[i]==3535);
    for(int i=513*2;i<600*2;i++) assert(out.pcm[i]==0);
    assert(out.before==1234 && out.after==5678);
    assert(atomic_load(&a.q_head)==3 && atomic_load(&a.rel_tail)==3);
    for(int i=0;i<3;i++) assert(a.rel[i]==&b[i]);
    assert(a.pos==0);
    // 暂停时不消费输入，输出静音。 | When stopped, preserve queued input and output silence.
    atomic_store(&a.q_head,0); atomic_store(&a.rel_tail,0); atomic_store(&a.state,STATE_STOPPED);
    memset(out.pcm,0x55,sizeof out.pcm);
    assert(au_render(&a,NULL,NULL,0,600,&bl)==noErr);
    for(int i=0;i<600*2;i++) assert(out.pcm[i]==0);
    assert(atomic_load(&a.q_head)==0 && atomic_load(&a.rel_tail)==0);
    // 双声道路径逐字节不变。 | Stereo path remains byte-for-byte unchanged.
    a.ch=2; atomic_store(&a.state,STATE_STARTED); atomic_store(&a.q_tail,1);
    int16_t stereo[4]={-32768,32767,1234,-5678}; b[0]=(gbuf){stereo,sizeof stereo,sizeof stereo};
    bl.mBuffers[0].mDataByteSize=sizeof stereo;
    assert(au_render(&a,NULL,NULL,0,2,&bl)==noErr);
    assert(!memcmp(out.pcm,stereo,sizeof stereo));
    dispatch_release(a.notify_sem);
    puts("Audio render: center routing, buffer boundaries, underrun, stop and stereo passed");
    return 0;
}
