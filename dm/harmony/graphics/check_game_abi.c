/* Host-only ABI/recording test. These hooks do not execute a GPU and are never
 * linked into xbg_game. Keep the fixture separate from the product backend. */
#include "xbg_game_internal.h"
#include "../compat-r32/gfx/nvn/nvn_protos.h"
void xbg_logf(const char *f,...){(void)f;}
__attribute__((noreturn)) void xbg_fatalf(const char *f,...){(void)f;__builtin_trap();}
int xbg_backend_failed(void){return 0;}
int xbg_device_integer(int p,int *v){(void)p;(void)v;return 0;}
void xbg_legacy_pool_add(void *p,size_t n){(void)p;(void)n;}
void xbg_legacy_pool_del(void *p){(void)p;}
void xbg_register_texture(const dpool_t *p,int id,const tex_t *t,const view_t *v){(void)p;(void)id;(void)t;(void)v;}
void xbg_register_sampler(const dpool_t *p,int id,const samp_par *s){(void)p;(void)id;(void)s;}
void xbg_legacy_tex_release(tex_t *p){(void)p;}
void xbg_legacy_prog_release(prog_t *p){(void)p;}
void xbg_texture_invalidate(const tex_t *p){(void)p;}
void xbg_legacy_read_texels(const tex_t *t,unsigned l,unsigned a,const NVNcopyRegion *r,uint8_t *d,size_t row,size_t image){(void)t;(void)l;(void)a;(void)r;(void)d;(void)row;(void)image;}
void *xbg_host_command_memory(cmd_t *c,size_t n){(void)c;(void)n;__builtin_trap();}
void xbg_release_command_handle(uint64_t h){(void)h;}
void xbg_invoke_command_callback(cmd_t *c,size_t n){c->cb((NVNcommandBuffer *)c,NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY,n,c->cb_data);}
void xbg_submit(int n,const uint64_t *h){(void)n;(void)h;__builtin_trap();}
void xbg_present_texture(const win_t *w,const tex_t *t){(void)w;(void)t;__builtin_trap();}
uint64_t xbg_legacy_last_fence(void){return 0;}
int xbg_legacy_wait(uint64_t f,uint64_t t){(void)t;return f==0;}
void xbg_legacy_finish(void){}
#define FN(n) ((NVNT_##n)xbg_nvn_get_proc_address(#n))
static int callbacks;
static unsigned char memory[4096];
static NVNmemoryPool pool;
static size_t available(void *userdata,const void *p){
    (void)userdata;uintptr_t a=(uintptr_t)p,b=(uintptr_t)memory;
    return a>=b && a<b+sizeof(memory)?b+sizeof(memory)-a:0;
}
void xbg_copy_commands(cmd_t *command,int count,const uint64_t *handles){
    static unsigned char snapshot[4096];size_t bytes=0;
    if(xbg_command_clone(count,handles,snapshot,sizeof(snapshot),available,0,&bytes))__builtin_trap();
    for(size_t off=0;off<bytes;){op_hdr h;memcpy(&h,snapshot+off,8);xbg_record_command(command,snapshot+off,h.size);off+=h.size;}
}
static void replenish(NVNcommandBuffer *c,NVNcommandBufferMemoryEvent event,size_t n,void *data){
    (void)data;if(event!=NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY || n>2048)__builtin_trap();
    ++callbacks;FN(nvnCommandBufferAddCommandMemory)(c,&pool,2048,2048);
}
int xbg_game_abi_selftest(void){
    NVNmemoryPoolBuilder pb={0};NVNcommandBuffer c={0};
    FN(nvnMemoryPoolBuilderSetDefaults)(&pb);FN(nvnMemoryPoolBuilderSetStorage)(&pb,memory,sizeof(memory));
    if(!FN(nvnMemoryPoolInitialize)(&pool,&pb))return 1;
    if(!FN(nvnCommandBufferInitialize)(&c,0))return 2;
    FN(nvnCommandBufferSetMemoryCallback)(&c,replenish);
    FN(nvnCommandBufferAddCommandMemory)(&c,&pool,0,96);
    FN(nvnCommandBufferBeginRecording)(&c);
    uint32_t data[4]={0x12345678,2,3,4},destination[4]={0};
    FN(nvnCommandBufferUpdateUniformBuffer)(&c,(NVNbufferAddress)destination,sizeof(destination),0,sizeof(data),data);
    data[0]=0; // recording must retain the original value
    FN(nvnCommandBufferDispatchCompute)(&c,3,4,5); // forces real guest callback/new segment
    FN(nvnCommandBufferBindUniformBuffer)(&c,NVN_SHADER_STAGE_FRAGMENT,2,0x12340000,256);
    NVNcommandHandle handle=FN(nvnCommandBufferEndRecording)(&c);
    if(callbacks!=1 || FN(nvnCommandBufferIsRecording)(&c))return 3;
    const uint8_t *p=(const uint8_t *)handle;op_hdr h;memcpy(&h,p,8);if(h.op!=OP_BEGIN)return 4;p+=h.size;
    memcpy(&h,p,8);if(h.op!=OP_UPDATE_UB)return 5;uint32_t stored;memcpy(&stored,p+24,4);if(stored!=0x12345678)return 6;p+=h.size;
    memcpy(&h,p,8);if(h.op!=OP_JUMP)return 7;memcpy(&p,p+8,8);
    memcpy(&h,p,8);if(h.op!=OP_DISPATCH)return 8;p_dispatch d;memcpy(&d,p+8,sizeof(d));if(d.x!=3||d.y!=4||d.z!=5)return 9;p+=h.size;
    memcpy(&h,p,8);if(h.op!=OP_BIND_UBO)return 10;p_bind_buf b;memcpy(&b,p+8,sizeof(b));if(b.stage!=1||b.index!=2||b.addr!=0x12340000||b.size!=256)return 11;p+=h.size;
    memcpy(&h,p,8);if(h.op!=OP_END)return 12;
    if(xbg_nvn_get_proc_address("not_an_NVN_function"))return 13;
    // Slots queried but not called must exist without pretending they execute.
    if(!xbg_nvn_get_proc_address("nvnDeviceWalkDebugDatabase"))return 14;
    // Copy the real recorded source, then immediately recycle its segments.
    NVNcommandBuffer copy={0};FN(nvnCommandBufferInitialize)(&copy,0);
    FN(nvnCommandBufferAddCommandMemory)(&copy,&pool,512,1024);
    FN(nvnCommandBufferBeginRecording)(&copy);
    FN(nvnCommandBufferCopyCommands)(&copy,1,&handle);
    uint8_t *copied=(uint8_t *)FN(nvnCommandBufferEndRecording)(&copy);
    memset(memory,0,96);memset(memory+2048,0,256);
    memcpy(&h,copied,8);if(h.op!=OP_BEGIN)return 15;copied+=h.size;
    memcpy(&h,copied,8);if(h.op!=OP_UPDATE_UB)return 16;memcpy(&stored,copied+24,4);if(stored!=0x12345678)return 17;copied+=h.size;
    memcpy(&h,copied,8);if(h.op!=OP_DISPATCH)return 18;copied+=h.size;
    memcpy(&h,copied,8);if(h.op!=OP_BIND_UBO)return 19;copied+=h.size;
    memcpy(&h,copied,8);if(h.op!=OP_END)return 20;
    // A nested self-CALL is bounded, and truncated/outside records are rejected.
    uint64_t cyclic=(uint64_t)(memory+3000);h=(op_hdr){OP_CALL,16};
    memcpy(memory+3000,&h,8);memcpy(memory+3008,&cyclic,8);size_t bytes=0;
    if(xbg_command_clone(1,&cyclic,0,4096,available,0,&bytes)!=-1)return 21;
    h=(op_hdr){OP_DISPATCH,4096};memcpy(memory+3000,&h,8);
    if(xbg_command_clone(1,&cyclic,0,4096,available,0,&bytes)!=-4)return 22;
    return 0;
}
