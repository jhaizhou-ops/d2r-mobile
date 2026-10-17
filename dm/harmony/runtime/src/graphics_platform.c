/* Original sw/gfx/hle_nv.c and hle_vi.c ABI, backed by the configured Vulkan
 * renderer/present path. This replaces those Nintendo driver/VI services only;
 * it does not make arbitrary Service Manager requests succeed. */
#include "hle_ohos_compat.h"

typedef void *(*nv_alloc_fn)(unsigned long,unsigned long,void *);
typedef void (*nv_free_fn)(void *,void *);
typedef void *(*nv_realloc_fn)(void *,unsigned long,void *);
typedef struct nv_allocator {nv_alloc_fn alloc;nv_free_fn free;nv_realloc_fn realloc;void *user;} nv_allocator;
static nv_allocator graphics_allocator,devtools_allocator;
static pthread_mutex_t platform_lock=PTHREAD_MUTEX_INITIALIZER;
static int vi_ready,graphics_ready;
static struct {uint32_t magic;} display={0x50534944},layer={0x5259414c};
static struct {uint32_t magic;void *layer;} native_window={0x4e49574e,&layer};
static int layer_created;
static void require_backend(void) {
    if(!xblr_graphics_configured())xblr_graphics_fatal(NULL,"NV/VI called before Vulkan resolver configuration");
}
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_","record original guest graphics allocator",
          void,h_nv_SetAlloc,nv_alloc_fn a,nv_free_fn f,nv_realloc_fn r,void *u) {
    pthread_mutex_lock(&platform_lock);graphics_allocator=(nv_allocator){a,f,r,u};pthread_mutex_unlock(&platform_lock);
}
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_","record original guest devtools allocator",
          void,h_nv_SetDevAlloc,nv_alloc_fn a,nv_free_fn f,nv_realloc_fn r,void *u) {
    pthread_mutex_lock(&platform_lock);devtools_allocator=(nv_allocator){a,f,r,u};pthread_mutex_unlock(&platform_lock);
}
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm","Vulkan owns real host GPU allocations; no Nintendo nvdrv connection",
          void,h_nv_Init,void *memory,unsigned long bytes) {
    require_backend();
    if(!memory||bytes<0x8000||(((uintptr_t)memory|bytes)&0xfff))xblr_graphics_fatal(NULL,"nv InitializeGraphics invalid work memory");
    pthread_mutex_lock(&platform_lock);graphics_ready=1;pthread_mutex_unlock(&platform_lock);
    LOG("nv graphics initialized via configured Vulkan backend; guest work bytes=%lu",bytes);
}
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv","host backend lifecycle remains owned by app",void,h_nv_Fin,void) {
    pthread_mutex_lock(&platform_lock);graphics_ready=0;pthread_mutex_unlock(&platform_lock);
}
HLE_FORCE("_ZN2nn2vi10InitializeEv","VI resolves the app's configured Vulkan presentation path",void,h_vi_Init,void) {
    require_backend();pthread_mutex_lock(&platform_lock);vi_ready=1;pthread_mutex_unlock(&platform_lock);
    LOG("vi initialized via configured presentation callback");
}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE","typed host display for app presentation",Result,h_vi_Open,void **out) {
    if(!out)return RES_FAIL;pthread_mutex_lock(&platform_lock);int ready=vi_ready;
    if(ready)*out=&display;pthread_mutex_unlock(&platform_lock);return ready?RES_OK:RES_FAIL;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE","one app surface layer",Result,h_vi_Layer,void **out,void *d) {
    if(!out||d!=&display)return RES_FAIL;
    pthread_mutex_lock(&platform_lock);int ready=vi_ready&&!layer_created;
    if(ready){layer_created=1;*out=&layer;}pthread_mutex_unlock(&platform_lock);return ready?RES_OK:RES_FAIL;
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE","opaque VI token forwarded to the real present callback",Result,h_vi_Native,void **out,void *l) {
    if(!out||l!=&layer)return RES_FAIL;pthread_mutex_lock(&platform_lock);int ready=vi_ready&&layer_created;
    if(ready)*out=&native_window;pthread_mutex_unlock(&platform_lock);return ready?RES_OK:RES_FAIL;
}
