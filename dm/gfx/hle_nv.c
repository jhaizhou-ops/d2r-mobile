// nv::（NVN 下面的显卡驱动胶水层：建传输内存、连 nvdrv 服务）。NVN 已整块换成宿主空设备，这层无事可做。
// 分配器回调记在宿主全局 g_nv_alloc，图形层以后要按游戏的分配器分内存时从这里取。
// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). NVN has been replaced wholesale with a host null device, so this layer has nothing to do.
// The allocator callbacks are recorded in the host global g_nv_alloc; the graphics layer takes them from here later when it needs to allocate memory with the game's allocator.
#include "../host/hle.h"

typedef void *(*nv_alloc_fn)(unsigned long size, unsigned long align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, unsigned long size, void *user);
struct nv_allocator { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; };
struct nv_allocator g_nv_alloc, g_nv_devtools_alloc;

#ifdef __ANDROID__
#include "../android/nvn_platform_backend.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
/* NvRmChannelOpen checks the signed result and returns its original 0x30012
 * driver-unavailable error. Use the Android device namespace's real outcome;
 * no Tegra channel or successful decoder is fabricated here. */
HLE_FORCE("NvOsDrvOpen", "Android device open reports real absence of the Tegra video driver", int,h_nv_DrvOpen,const char *path) {
    if(!path || strncmp(path,"/dev/",5))die("Android NvOsDrvOpen invalid device path %s",path ? path : "(null)");
    int fd=open(path,O_RDWR|O_CLOEXEC),saved_errno=errno;
    LOG("[nv-android] real driver open %s result=%d errno=%d",path,fd,fd<0 ? saved_errno : 0);
    if(fd>=0){close(fd);die("Android NvOsDrvOpen found device %s; Tegra ioctl translation is required",path);}
    errno=saved_errno;
    return -1;
}
/* The original video implementation returns NvError 2 at 77b10 when its
 * channel cannot be opened, but its error logger dereferences absolute 1c98
 * before returning. Report that same service failure before constructing a
 * Tegra block. The original caller retains its own failure/fallback handling. */
HLE_FORCE("NvMMLiteOpenVideo", "Absent Android Tegra video device returns original channel-open error without creating a block", unsigned,h_nv_OpenVideo,unsigned type,void *block,void *params,void *output) {
    (void)block;(void)params;(void)output;
    int fd=open("/dev/nvhost-ctrl",O_RDWR|O_CLOEXEC),error=errno;
    if(fd>=0){close(fd);die("Android Tegra video block requires real decoder implementation");}
    if(error!=ENOENT && error!=ENODEV && error!=EACCES)die("Android video device unexpected error %d",error);
    LOG("[nv-android] NvMMLiteOpenVideo type=%#x driver unavailable errno=%d original_error=2",type,error);
    return 2;
}
#define WHY "Android NVN uses an actual Vulkan device and registered original allocator/session memory"
static void android_nv_allocator(struct nv_allocator *dst,nv_alloc_fn a,nv_free_fn f,nv_realloc_fn r,void *u) {
    if(dm_vk_graphics_live())die("Android NVN allocator changed during initialized graphics session");
    if(!a || !f)die("Android NVN allocator requires allocation and free callbacks");
    *dst=(struct nv_allocator){a,f,r,u};
    LOG("[nv-android] allocator registered alloc=%p free=%p realloc=%p user=%p",(void *)a,(void *)f,(void *)r,u);
}
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY,void,h_nv_SetAlloc,nv_alloc_fn a,nv_free_fn f,nv_realloc_fn r,void *u) {
    android_nv_allocator(&g_nv_alloc,a,f,r,u);
}
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY,void,h_nv_SetDevAlloc,nv_alloc_fn a,nv_free_fn f,nv_realloc_fn r,void *u) {
    android_nv_allocator(&g_nv_devtools_alloc,a,f,r,u);
}
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY,void,h_nv_Init,void *mem,unsigned long size) {
    if(!g_nv_alloc.alloc || !g_nv_alloc.free)die("Android nv::InitializeGraphics missing original graphics allocator");
    dm_vk_graphics_begin(mem,size);
}
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY,void,h_nv_Fin,void) {
    dm_vk_graphics_end();memset(&g_nv_alloc,0,sizeof g_nv_alloc);memset(&g_nv_devtools_alloc,0,sizeof g_nv_devtools_alloc);
}
#else
#define WHY "NVN 已整块换成宿主空设备，驱动胶水层无事可做"
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "；分配器记入 g_nv_alloc",
          void, h_nv_SetAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "；分配器记入 g_nv_devtools_alloc",
          void, h_nv_SetDevAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY, void, h_nv_Init, void *mem, unsigned long size) { (void)mem; (void)size; }
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY, void, h_nv_Fin, void) {}
#endif
