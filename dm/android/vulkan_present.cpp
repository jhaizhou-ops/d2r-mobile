// 安卓Vulkan呈现与窗口生命周期；依据原WSI桥及PR9/10的可回退呈现/钉核实验。
// Android Vulkan presentation and window lifetimes, based on the original WSI bridge and reversible PR9/10 overlap/affinity experiments.
#define VK_USE_PLATFORM_ANDROID_KHR
#include "vulkan_present.h"
#include "../gfx/vulkan/nvn_vk.h"
#include "../gfx/xtrace.h"
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sched.h>
#include <unistd.h>
#include <time.h>
extern "C" double dm_present_period(void);
// 错过上屏节拍的帧数（晚于「上次期望时刻 + 周期」2 ms 以上）；自适应出图档按它降档 / 试升
// Frames that missed their display slot (more than 2 ms later than "previous desired time + period"); the adaptive present tier steps down / probes up by it
static std::atomic<uint64_t> dm_present_late{0};
extern "C" uint64_t dm_present_late_count(void){return dm_present_late.load(std::memory_order_relaxed);}
namespace {
// 图形日志写 stderr：jni_android.c 已把它接到 logcat 标签 dm 与日志文件，玩家按 `adb logcat -s dm` 或拉日志文件都能拿全。
// 以前单独用标签 dm-vulkan，-s dm 抓不到「NVN caps」「gfx-pipe」这些定位驱动问题的行。整行一次写出，多线程不交错。
// Graphics log lines go to stderr: jni_android.c connects it to the logcat tag dm and the log file, so `adb logcat -s dm` or the
// log file has them all. They used to have their own tag dm-vulkan, which -s dm missed, along with the "NVN caps" and
// "gfx-pipe" lines that pin down driver problems. Each line is written in one call, so threads do not interleave.
__attribute__((format(printf,1,2))) void vlog(const char *format,...){
    char text[1100];va_list a;va_start(a,format);int n=vsnprintf(text,sizeof text-1,format,a);va_end(a);
    if(n<0)return;size_t len=std::min(size_t(n),sizeof text-2);text[len]='\n';fwrite(text,1,len+1,stderr);
}
std::mutex window_mutex;std::condition_variable window_cv;
// Vulkan状态仅宿主线程使用；UI锁只保护窗口快照，不跨GPU等待。
// Serialize Vulkan callers separately; never hold the UI window lock across GPU waits.
std::mutex render_mutex;
ANativeWindow *desired=nullptr,*active=nullptr;uint64_t generation=0,active_generation=0;
VkInstance instance;VkPhysicalDevice physical;VkDevice device;VkQueue queue;uint32_t family;
VkSurfaceKHR surface;VkSwapchainKHR swapchain;VkFormat swap_format;VkExtent2D extent;
VkSurfaceTransformFlagBitsKHR created_transform;VkExtent2D created_surface_extent;
// 无旋转 blit 由系统合成器旋转；记录属性以免 SUBOPTIMAL 触发逐帧重建。
// The compositor rotates our unrotated blit; cache properties to avoid rebuilding every SUBOPTIMAL frame.
bool surface_properties_changed(){
    VkSurfaceCapabilitiesKHR c{};
    if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&c)!=VK_SUCCESS)return true;
    return c.currentTransform!=created_transform || c.currentExtent.width!=created_surface_extent.width || c.currentExtent.height!=created_surface_extent.height;
}
std::vector<VkImage> images;VkCommandPool command_pool;VkCommandBuffer command;
VkSemaphore acquired;std::vector<VkSemaphore> finished;VkFence fence;bool ready=false,swap_dirty=false;std::string shader_path;
// 缺省开启（标记文件 gfx-present-overlap 为 '0' 时关）：最多两次呈现 blit 在途，不在每帧后清空队列，游戏可在 GPU 收尾本帧时翻译下一帧。
// On by default (marker file gfx-present-overlap = '0' turns it off): keep up to two present blits in flight instead of draining the queue
// after every frame, so the game translates the next frame while the GPU finishes this one. Each in-flight frame has
// its own pool, acquire semaphore and fence; slot 0 is the original set. Later engine barriers on the source image are
// ALL_COMMANDS and its release retires with a later submission, which covers this blit in queue order.
bool overlap=false;VkCommandPool overlap_pool;VkCommandBuffer overlap_command;VkSemaphore overlap_acquired;VkFence overlap_fence;
bool in_flight[2];unsigned flight=0;
// Diagnostic (marker gfx-present-timing; overlap mode): GPU time of each present's clear and blit, from two timestamps per
// in-flight frame, read once that frame's fence has signalled; averaged over 300 presents.
bool present_timing=false;VkQueryPool present_queries=VK_NULL_HANDLE;bool present_stamped[2];double present_tick_ns=1,present_gpu_ns=0;unsigned present_timed=0;
// 游戏线程（调用呈现的线程；同步翻译时也是翻译线程）钉在最高频的核上。Redmi K50 Ultra 上调度器一直把它放在 A710
// （cpu4–6，MIUI 封顶 2.57 GHz），从不用空着的 X2（cpu7，封顶 2.25 GHz）；钉到 X2 后营地在 3D 比例 0.6 下从 40.2 升到
// 44.3 fps（同进程 A/B，CPU 17.1 → 14.3 ms）。MIUI 会改线程的可用核掩码，所以每 120 次呈现核对一次。
// 标记 gfx-no-pin 关闭，gfx-pin-cpu = 十六进制掩码 指定核；gfx-ab-toggle = N 加 gfx-ab-target = pin 时每 N 秒钉 / 放开交替。
// The game thread (the caller of present, which also translates in sync mode) runs on the cores with the highest maximum
// frequency. On the Redmi K50 Ultra the scheduler kept it on the A710 cores (cpu4-6, capped at 2.57 GHz by MIUI) and never
// used the idle X2 (cpu7, capped at 2.25 GHz); pinned to the X2 the camp went from 40.2 to 44.3 fps at 3D scale 0.6
// (in-process A/B, CPU 17.1 -> 14.3 ms). MIUI rewrites thread affinity masks, so the pin is checked every 120 presents.
// Marker gfx-no-pin turns it off and gfx-pin-cpu = hex mask picks the cores; gfx-ab-toggle = N with gfx-ab-target = pin
// alternates pinned and released every N seconds, in phase with the engine's A/B (its PERF lines carry ab=).
unsigned pin_mask=0;int pin_ab=0;int pin_state=-1;unsigned pin_presents=0,pin_drifts=0,pin_failures=0;cpu_set_t pin_original;bool pin_have_original=false;
unsigned fastest_cores(){
    unsigned best=0,mask=0,present=0;
    for(int c=0;c<32;c++){char path[96];snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",c);FILE *f=fopen(path,"rb");if(!f)continue;
        unsigned v=0;if(fscanf(f,"%u",&v)==1 && v){present|=1u<<c;if(v>best){best=v;mask=0;}if(v==best)mask|=1u<<c;}fclose(f);}
    return mask==present?0:mask;   // one core class: nothing to pick
}
void pin_update(){
    if(!pin_mask)return;bool want=true;
    if(pin_ab>0){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);want=((ts.tv_sec/pin_ab)&1)==0;}
    bool changed=int(want)!=pin_state;if(!changed && ++pin_presents%120)return;
    if(!pin_have_original){CPU_ZERO(&pin_original);if(sched_getaffinity(0,sizeof pin_original,&pin_original))for(int c=0;c<32;c++)CPU_SET(c,&pin_original);pin_have_original=true;}
    cpu_set_t set;CPU_ZERO(&set);if(want){for(int c=0;c<32;c++)if(pin_mask>>c&1)CPU_SET(c,&set);}else set=pin_original;
    if(!changed){cpu_set_t cur;CPU_ZERO(&cur);if(!sched_getaffinity(0,sizeof cur,&cur) && CPU_EQUAL(&cur,&set))return;++pin_drifts;}
    int r=sched_setaffinity(0,sizeof set,&set),err=r?errno:0;pin_state=want;if(r)++pin_failures;
    // Refused while the app's cpuset lacks the cores (Android moves the app between cpusets, at start for one): the kernel
    // keeps the request and applies it again when the cpuset allows, and the next check retries.
    if(changed || pin_drifts<=4 || (r && pin_failures<=4))vlog("NVN pin tid=%d %s mask=%x result=%d errno=%d drifts=%u failures=%u",int(gettid()),want?"pinned":"released",want?pin_mask:0u,r,err,pin_drifts,pin_failures);
}
void log_message(void*,const char *text){vlog("%s",text);}
[[noreturn]] void fatal(void*,const char *text){vlog("%s",text);__android_log_print(ANDROID_LOG_FATAL,"dm","%s",text);abort();}
void destroy_present_semaphores(){for(auto s:finished)if(s)vkDestroySemaphore(device,s,nullptr);finished.clear();}
// 调用者先等队列空闲；基础WSI销毁仍沿用WaitIdle惯例，不声称提供呈现栅栏保证。
// Call only after queue idle. Base WSI teardown follows the WaitIdle convention,
// which is not the explicit presentation-fence guarantee of swapchain_maintenance1.
void destroy_swap(){destroy_present_semaphores();if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;images.clear();if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);surface=VK_NULL_HANDLE;if(active)ANativeWindow_release(active);active=nullptr;}
void cleanup_initialization(){
    if(queue)vkQueueWaitIdle(queue);
    if(device){if(fence)vkDestroyFence(device,fence,nullptr);if(acquired)vkDestroySemaphore(device,acquired,nullptr);if(command_pool)vkDestroyCommandPool(device,command_pool,nullptr);}
    fence=VK_NULL_HANDLE;acquired=VK_NULL_HANDLE;command_pool=VK_NULL_HANDLE;command=VK_NULL_HANDLE;
    if(device){if(overlap_fence)vkDestroyFence(device,overlap_fence,nullptr);if(overlap_acquired)vkDestroySemaphore(device,overlap_acquired,nullptr);if(overlap_pool)vkDestroyCommandPool(device,overlap_pool,nullptr);}
    overlap_fence=VK_NULL_HANDLE;overlap_acquired=VK_NULL_HANDLE;overlap_pool=VK_NULL_HANDLE;overlap_command=VK_NULL_HANDLE;overlap=false;in_flight[0]=in_flight[1]=false;flight=0;
    destroy_swap();if(device)vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;queue=VK_NULL_HANDLE;
    if(instance)vkDestroyInstance(instance,nullptr);instance=VK_NULL_HANDLE;physical=VK_NULL_HANDLE;
}
// 刷新率投票：App 不投票时，安卓（MIUI）让这个表面跑 60 Hz，尽管屏和系统设置都允许 120。投 120 Hz 不改 60 帧的出图节奏（nvn_pace_hz），
// 但赶不上 16.7 ms 的那一帧在屏上停 25 ms 而不是 33.3 ms。标记 gfx-frame-rate = N（Hz；0 = 不投票）。
// Frame-rate vote: without a vote from the app, Android (MIUI) runs this surface at 60 Hz although the panel and the system
// setting allow 120. A 120 Hz vote keeps the 60 fps present pacing (nvn_pace_hz), but a frame that misses 16.7 ms stays on
// screen for 25 ms instead of 33.3. Marker gfx-frame-rate = N (Hz; 0 = no vote).
float frame_rate_vote=120;
// 按游戏输出尺寸出图：窗口缓冲设成游戏输出的尺寸（2000×900），由显示硬件放大到屏幕，呈现端只做 1:1 拷贝，不再每帧清屏、
// 用 GPU 按最近邻放大到 2712×1220。分辨率不变，最后一步放大从 GPU 移到显示控制器（滤波也更平滑）。标记 gfx-present-native = 0 关。
// Present at the game's output size: the window's buffers take the game's output size (2000x900) and the display hardware
// scales them to the screen, so the presenter copies 1:1 instead of clearing and nearest-scaling to 2712x1220 on the GPU
// every frame. Same resolution; the final upscale moves from the GPU to the display controller (with smoother filtering).
// Marker gfx-present-native = 0 turns it off.
#ifdef __OHOS__
bool present_native=false;   // 鸿蒙：缓冲尺寸与合成缩放未在真机核过，先关；标记 gfx-present-native = 1 打开 | HarmonyOS: buffer geometry vs compositor scaling not yet checked on device; off, marker gfx-present-native = 1 enables
#else
bool present_native=true;
#endif
int native_w=0,native_h=0;
int create_surface(){
    destroy_swap();native_w=native_h=0;
    {std::lock_guard<std::mutex> lock(window_mutex);if(!desired)return VK_NOT_READY;active=desired;ANativeWindow_acquire(active);active_generation=generation;}
    if(frame_rate_vote>0){
        // 运行时查询API，系统拒绝投票不会中止Vulkan初始化。 | Resolve the API at runtime; a refused vote does not abort Vulkan initialization.
        static auto set_rate=[]{void *lib=dlopen("libnativewindow.so",RTLD_NOW);return reinterpret_cast<int32_t(*)(ANativeWindow*,float,int8_t)>(lib?dlsym(lib,"ANativeWindow_setFrameRate"):nullptr);}();
        int fr=set_rate?set_rate(active,frame_rate_vote,ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT):-1;
        vlog("Frame rate vote %.0f Hz result=%d (marker gfx-frame-rate; display request, not FPS)",double(frame_rate_vote),fr);
    }
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};ci.window=active;
    VkResult r=vkCreateAndroidSurfaceKHR(instance,&ci,nullptr,&surface);if(r)return r;
    if(!device)return 0;VkBool32 supported=VK_FALSE;r=vkGetPhysicalDeviceSurfaceSupportKHR(physical,family,surface,&supported);if(r||!supported)return r?r:VK_ERROR_FEATURE_NOT_PRESENT;return 0;
}
int create_swap(){
    VkSurfaceCapabilitiesKHR cap;VkResult r=vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&cap);if(r)return r;
    if(!(cap.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!(cap.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t n=0;r=vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,nullptr);if(r||!n)return r?r:VK_ERROR_FORMAT_NOT_SUPPORTED;
    std::vector<VkSurfaceFormatKHR> formats(n);r=vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,formats.data());if(r)return r;
    VkSurfaceFormatKHR sf=formats[0];if(sf.format==VK_FORMAT_UNDEFINED)sf.format=VK_FORMAT_R8G8B8A8_UNORM;
    for(auto f:formats)if(f.format==VK_FORMAT_R8G8B8A8_UNORM||f.format==VK_FORMAT_B8G8R8A8_UNORM){sf=f;break;}
    extent=cap.currentExtent;if(extent.width==UINT32_MAX){extent.width=std::clamp(uint32_t(std::max(1,ANativeWindow_getWidth(active))),cap.minImageExtent.width,cap.maxImageExtent.width);extent.height=std::clamp(uint32_t(std::max(1,ANativeWindow_getHeight(active))),cap.minImageExtent.height,cap.maxImageExtent.height);}if(!extent.width||!extent.height)return VK_NOT_READY;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;
#ifdef __OHOS__
    {unsigned want=4;if(const char *e=getenv("DM_SWAP_IMAGES"))want=unsigned(std::max(2,atoi(e)));ci.minImageCount=std::max(want,cap.minImageCount);}   // 马良最佳实践：重负载 3–4 张；按期望上屏时刻排帧时合成器手里多压一张，取 4 张免得翻译线程等交换链、反压到游戏帧节奏 | Maleoon best practice: 3–4 under heavy load; with desired display times the compositor holds one more buffer, so take 4 to keep the translation thread from waiting on the swapchain and back-pressuring the game cadence
#else
    ci.minImageCount=std::max(2u,cap.minImageCount);
#endif
    if(cap.maxImageCount)ci.minImageCount=std::min(ci.minImageCount,cap.maxImageCount);ci.imageFormat=sf.format;ci.imageColorSpace=sf.colorSpace;ci.imageExtent=extent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(cap.supportedCompositeAlpha&ci.compositeAlpha))for(uint32_t b=1;b<=8;b<<=1)if(cap.supportedCompositeAlpha&b){ci.compositeAlpha=VkCompositeAlphaFlagBitsKHR(b);break;}
    ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;ci.oldSwapchain=swapchain;VkSwapchainKHR replacement=VK_NULL_HANDLE;r=vkCreateSwapchainKHR(device,&ci,nullptr,&replacement);
    // 创建失败也会退休oldSwapchain；重试不得继续传入退休句柄。
    // Even failed creation retires oldSwapchain; never pass that retired handle on retry.
    destroy_present_semaphores();if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=r?VK_NULL_HANDLE:replacement;images.clear();if(r)return r;swap_format=sf.format;
    created_transform=cap.currentTransform;created_surface_extent=cap.currentExtent;
    vlog("Swapchain: window=%dx%d extent=%ux%u currentTransform=%u supported=%u selected=%u",ANativeWindow_getWidth(active),ANativeWindow_getHeight(active),extent.width,extent.height,unsigned(cap.currentTransform),unsigned(cap.supportedTransforms),unsigned(ci.preTransform));
    r=vkGetSwapchainImagesKHR(device,swapchain,&n,nullptr);if(r)return r;images.resize(n);r=vkGetSwapchainImagesKHR(device,swapchain,&n,images.data());if(r)return r;images.resize(n);
    // 呈现完成信号量按交换链图像分配；重新获取该图像后才能再次发信号。
    // Index present-wait semaphores by acquired image, not by CPU frame/fence.
    finished.resize(n,VK_NULL_HANDLE);VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(auto &s:finished){r=vkCreateSemaphore(device,&sc,nullptr,&s);if(r)return r;}return VK_SUCCESS;
}
// 诊断（标记 gfx-membench）：每种主机可见内存类型的 CPU 写、CPU 读，以及 GPU 填充后的 CPU 读速度，启动时测一次。
// Diagnostic (marker gfx-membench): CPU write, CPU read, and CPU read after a GPU fill, for each host-visible memory type; run once at start.
void membench(){
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    const VkDeviceSize n=4u<<20;std::vector<uint8_t> host(n,1),back(n);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;VkCommandPool pool;if(vkCreateCommandPool(device,&cp,nullptr,&pool))return;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd;if(vkAllocateCommandBuffers(device,&ca,&cmd)){vkDestroyCommandPool(device,pool,nullptr);return;}
    auto now=[]{timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return double(ts.tv_sec)*1e9+double(ts.tv_nsec);};
    for(uint32_t t=0;t<mp.memoryTypeCount;t++){
        VkMemoryPropertyFlags f=mp.memoryTypes[t].propertyFlags;if(!(f&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))continue;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=n;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;VkBuffer b;if(vkCreateBuffer(device,&bi,nullptr,&b))continue;
        VkMemoryRequirements r;vkGetBufferMemoryRequirements(device,b,&r);if(!(r.memoryTypeBits&(1u<<t))){vkDestroyBuffer(device,b,nullptr);continue;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=t;VkDeviceMemory m;if(vkAllocateMemory(device,&ai,nullptr,&m)){vkDestroyBuffer(device,b,nullptr);continue;}
        void *p=nullptr;if(vkBindBufferMemory(device,b,m,0)||vkMapMemory(device,m,0,VK_WHOLE_SIZE,0,&p)){vkFreeMemory(device,m,nullptr);vkDestroyBuffer(device,b,nullptr);continue;}
        bool coherent=(f&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};mr.memory=m;mr.size=VK_WHOLE_SIZE;
        double w=1e30,rc=1e30,rd=1e30,flush=0,inv=0;bool ok=true;
        for(int k=0;k<4;k++){double t0=now();memcpy(p,host.data(),n);double t1=now();if(!coherent)vkFlushMappedMemoryRanges(device,1,&mr);double t2=now();w=std::min(w,t1-t0);flush=std::max(flush,t2-t1);}
        for(int k=0;k<4;k++){double t0=now();memcpy(back.data(),p,n);rc=std::min(rc,now()-t0);}
        for(int k=0;k<4;k++){
            vkResetCommandBuffer(cmd,0);VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};vkBeginCommandBuffer(cmd,&cb);
            uint32_t value=0x01020304u+uint32_t(k);vkCmdFillBuffer(cmd,b,0,n,value);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
            vkEndCommandBuffer(cmd);VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;if(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)||vkQueueWaitIdle(queue)){ok=false;break;}
            double t0=now();if(!coherent)vkInvalidateMappedMemoryRanges(device,1,&mr);double t1=now();memcpy(back.data(),p,n);double t2=now();
            rd=std::min(rd,t2-t1);inv=std::max(inv,t1-t0);uint32_t got;memcpy(&got,back.data()+n-4,4);ok=ok&&got==value;
        }
        vlog("NVN membench type=%u flags=%#x write=%.2f GB/s (flush %.0f us) read=%.2f GB/s read_after_gpu=%.2f GB/s (invalidate %.0f us) ok=%d",
            t,unsigned(f),double(n)/w,flush/1e3,double(n)/rc,double(n)/rd,inv/1e3,int(ok));
        vkUnmapMemory(device,m);vkFreeMemory(device,m,nullptr);vkDestroyBuffer(device,b,nullptr);
    }
    vkDestroyCommandPool(device,pool,nullptr);
}
int has_window(void*){std::lock_guard<std::mutex> l(window_mutex);return desired!=nullptr;}
void wait_window(){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait(lock,[]{return desired!=nullptr;});}
bool window_changed(){std::lock_guard<std::mutex> lock(window_mutex);return !desired||generation!=active_generation;}
void wait_window_change(uint64_t bad){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait(lock,[bad]{return generation!=bad;});}
void wait_resize(){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait_for(lock,std::chrono::milliseconds(16),[]{return generation!=active_generation;});}
int present(void*,void*,VkImage source,VkFormat source_format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t){
    std::lock_guard<std::mutex> render_lock(render_mutex);
    pin_update();
    VkFormatProperties sp;vkGetPhysicalDeviceFormatProperties(physical,source_format,&sp);if(!(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    int x=0,y=0,w=int(width),h=int(height);if(crop&&crop[2]>0&&crop[3]>0){x=std::clamp(crop[0],0,w);y=std::clamp(crop[1],0,h);w=std::min(crop[2],int(width)-x);h=std::min(crop[3],int(height)-y);}if(w<=0||h<=0)return VK_ERROR_INITIALIZATION_FAILED;
    for(;;){
    if(present_native && surface && active && (native_w!=w || native_h!=h)){   // the next swapchain takes this size (currentExtent follows the buffer geometry)
        int g=ANativeWindow_setBuffersGeometry(active,w,h,0);native_w=w;native_h=h;swap_dirty=true;
        vlog("Present at the game's output size %dx%d result=%d (marker gfx-present-native)",w,h,g);}
    // 后台等待释放UI锁；保留同一源帧，恢复并真正呈现后才返回成功。
    // Suspend without the UI lock; keep this source frame until it is presented.
    wait_window();
    VkResult r;
    if(!surface||window_changed()){
        if((r=vkQueueWaitIdle(queue)))return r;
        r=VkResult(create_surface());
        if(r==VK_NOT_READY)continue;
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return r;swap_dirty=true;
    }
    if(swap_dirty){
        if((r=vkQueueWaitIdle(queue)))return r;
        r=VkResult(create_swap());
        if(r==VK_NOT_READY||r==VK_ERROR_OUT_OF_DATE_KHR){wait_resize();continue;}
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return r;swap_dirty=false;
    }
    VkFormatProperties dp;vkGetPhysicalDeviceFormatProperties(physical,swap_format,&dp);if(!(dp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    // 有限获取超时让渲染线程重新检查Surface代数，UI从不等待驱动。
    // A bounded acquire lets rendering observe lifecycle changes; UI never waits on the driver.
    unsigned f=overlap?flight:0;VkCommandPool pool=f?overlap_pool:command_pool;VkCommandBuffer cmd=f?overlap_command:command;VkSemaphore acq=f?overlap_acquired:acquired;VkFence done=f?overlap_fence:fence;
    if(in_flight[f]){XTRACE_BEGIN("dm-present-fence");r=vkWaitForFences(device,1,&done,VK_TRUE,10000000000ull);XTRACE_END();if(r)return r;if((r=vkResetFences(device,1,&done)))return r;in_flight[f]=false;
        if(present_stamped[f]){uint64_t t[2]{};if(vkGetQueryPoolResults(device,present_queries,f*2,2,sizeof t,t,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS && t[1]>=t[0]){present_gpu_ns+=double(t[1]-t[0])*present_tick_ns;
            if(++present_timed==300){vlog("NVN PERF present_gpu_us=%.0f extent=%ux%u (marker gfx-present-timing)",present_gpu_ns/present_timed/1e3,extent.width,extent.height);present_gpu_ns=0;present_timed=0;}}present_stamped[f]=false;}}
    uint32_t index=0;XTRACE_BEGIN("dm-acquire");r=vkAcquireNextImageKHR(device,swapchain,50000000ull,acq,VK_NULL_HANDLE,&index);XTRACE_END();
    if(r==VK_TIMEOUT||r==VK_NOT_READY)continue;
    if(r==VK_ERROR_OUT_OF_DATE_KHR){swap_dirty=true;wait_resize();continue;}
    if(r==VK_ERROR_SURFACE_LOST_KHR){if((r=vkQueueWaitIdle(queue)))return r;uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
    bool rebuild=r==VK_SUBOPTIMAL_KHR;if(r!=VK_SUCCESS&&!rebuild)return r;
    r=vkResetCommandPool(device,pool,0);if(r)return r;VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;if((r=vkBeginCommandBuffer(cmd,&bi)))return r;
    bool stamp=present_timing && overlap;if(stamp && !present_queries){VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=4;if(vkCreateQueryPool(device,&q,nullptr,&present_queries))stamp=false;
        VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(physical,&pp);present_tick_ns=pp.limits.timestampPeriod;}
    if(stamp){vkCmdResetQueryPool(cmd,present_queries,f*2,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,present_queries,f*2);}
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    // 换布局须在获取信号量的等待阶段（下面提交的 TRANSFER）之后：源阶段写 TOP_OF_PIPE 时，换布局不等信号量，可能在呈现引擎
    // 读完这张图之前就发生（验证层同步检查：SYNC-HAZARD-WRITE-AFTER-READ）。
    // The layout transition must come after the acquire semaphore's wait stage (TRANSFER, in the submit below). With
    // TOP_OF_PIPE as the source stage it does not wait for the semaphore and can happen before the presentation engine has
    // finished reading the image (synchronization validation: SYNC-HAZARD-WRITE-AFTER-READ).
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    double scale=std::min(double(extent.width)/w,double(extent.height)/h);int dw=std::max(1,int(w*scale)),dh=std::max(1,int(h*scale)),dx=(int(extent.width)-dw)/2,dy=(int(extent.height)-dh)/2;
    if(dx || dy || dw!=int(extent.width) || dh!=int(extent.height)){   // letterbox bars only where the blit leaves part of the image
        VkClearColorValue black{};vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&barrier.subresourceRange);
        barrier.oldLayout=barrier.newLayout;barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);}
    VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;blit.srcOffsets[0]={x,y,0};blit.srcOffsets[1]={x+w,y+h,1};blit.dstOffsets[0]={dx,dy,0};blit.dstOffsets[1]={dx+dw,dy+dh,1};
    // 最近邻不要求额外线性过滤能力。鸿蒙：游戏出图比屏幕小、要放大，格式支持时用线性，不然像素发硬、斜边锯齿。
    // Nearest needs no extra linear-filter support. HarmonyOS: the game image is smaller than the screen and gets upscaled; use linear when the format supports it, otherwise pixels look blocky and diagonals jagged.
    VkFilter filter=VK_FILTER_NEAREST;
#ifdef __OHOS__
    if(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)filter=VK_FILTER_LINEAR;
#endif
    vkCmdBlitImage(cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);
    // 同队列后续写入必须等源图读取完成；即使下一次图像布局未改变也建立 WAR 执行依赖。
    // Order subsequent same-queue writes after the source read, including reuse without a layout change.
    VkMemoryBarrier source_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};source_read.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;source_read.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&source_read,0,nullptr,0,nullptr);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    if(stamp){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,present_queries,f*2+1);present_stamped[f]=true;}
    if((r=vkEndCommandBuffer(cmd)))return r;VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acq;submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&finished[index];
    if((r=vkQueueSubmit(queue,1,&submit,done)))return r;in_flight[f]=true;VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&finished[index];pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;
#ifdef __OHOS__
    // 期望上屏时刻（鸿蒙 SET_DESIRED_PRESENT_TIMESTAMP）：上屏节奏 = 上次期望时刻 + 一个出图档周期；早到的帧由合成器按时放出，翻译线程不等；
    // 落后时从当前时刻重新起拍。翻译完成时刻的抖动（±5 ms）不再让一帧在屏上停 3 或 5 个刷新。DM_PRESENT_TS=0 关。
    // Desired display time (HarmonyOS SET_DESIRED_PRESENT_TIMESTAMP): cadence = previous desired time + one present-tier period; an early frame is released on time by the compositor
    // without the translation thread waiting; when late, restart the cadence from now. Jitter in translation completion (±5 ms) no longer leaves a frame on screen for 3 or 5 refreshes.
    {static int on=-1;if(on<0){const char *e=getenv("DM_PRESENT_TS");on=!(e&&*e=='0');}static int64_t tgt;double per=dm_present_period();
     if(on&&per>0&&active){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);int64_t now=int64_t(ts.tv_sec)*1000000000+ts.tv_nsec,p=int64_t(per*1e9),next=tgt?tgt+p:now;
      if(tgt&&now>tgt+p+2000000)dm_present_late.fetch_add(1,std::memory_order_relaxed);if(next<now)next=now;if(next>now+2*p)next=now+p;tgt=next;OH_NativeWindow_NativeWindowHandleOpt(active,SET_DESIRED_PRESENT_TIMESTAMP,next);}}
#endif
    XTRACE_BEGIN("dm-queue-present");VkResult pr=vkQueuePresentKHR(queue,&pi);XTRACE_END();
    // 呈现失败时先排空，拆除路径要求队列空闲。
    // Drain a failed present before teardown, which requires an idle queue.
    if(overlap){flight^=1;if(pr!=VK_SUCCESS && pr!=VK_SUBOPTIMAL_KHR && (r=vkQueueWaitIdle(queue)))return r;}
    else{if((r=vkWaitForFences(device,1,&done,VK_TRUE,10000000000ull)))return r;if((r=vkResetFences(device,1,&done)))return r;in_flight[0]=false;if((r=vkQueueWaitIdle(queue)))return r;}
    if(pr==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
    if(pr==VK_ERROR_OUT_OF_DATE_KHR){swap_dirty=true;wait_resize();continue;}
    if(pr!=VK_SUCCESS&&pr!=VK_SUBOPTIMAL_KHR)return pr;
    // 已成功呈现的帧不因尺寸瞬变报错；下帧再重建或等待可用尺寸。
    // Defer suboptimal recreation to the next frame, which can wait for a usable extent.
    if((rebuild||pr==VK_SUBOPTIMAL_KHR)&&surface_properties_changed())swap_dirty=true;
    return 0;
    }
}
}
extern "C" void dm_android_vk_set_window(ANativeWindow *window){if(window)ANativeWindow_acquire(window);{std::lock_guard<std::mutex> l(window_mutex);if(desired)ANativeWindow_release(desired);desired=window;++generation;}window_cv.notify_all();}
extern "C" int dm_android_vk_has_window(){return has_window(nullptr);}
extern "C" int dm_android_vk_initialize(const char *path,char *error,size_t bytes){
    if(path){std::string dir=path;size_t cut=dir.find_last_of('/');if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-native").c_str(),"rb")){present_native=fgetc(m)!='0';fclose(m);}
        if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-timing").c_str(),"rb")){present_timing=true;fclose(m);}}
    std::lock_guard<std::mutex> render_lock(render_mutex);
    auto fail=[&](const char *what,VkResult r)->int{if(error&&bytes)snprintf(error,bytes,"%s: VkResult %d",what,int(r));if(!ready)cleanup_initialization();return int(r?r:VK_ERROR_INITIALIZATION_FAILED);};
    if(ready||!path||!*path)return fail("Initialization requires an external shader directory and an uninitialized renderer",VK_ERROR_INITIALIZATION_FAILED);
    // 每次初始化从缺省的 120 Hz 重新读标记。 | Each initialization starts again from the 120 Hz default before the marker.
    frame_rate_vote=120;
    {std::string dir=path;size_t cut=dir.find_last_of('/');if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-frame-rate").c_str(),"rb")){float v=0;frame_rate_vote=fscanf(m,"%f",&v)==1 && v>=0 && v<=240?v:0;fclose(m);}}

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};ai.pApplicationName="dm-android";ai.apiVersion=VK_API_VERSION_1_1;
    const char *ext[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&ai;ic.enabledExtensionCount=2;ic.ppEnabledExtensionNames=ext;VkResult r=vkCreateInstance(&ic,nullptr,&instance);if(r)return fail("vkCreateInstance",r);
    uint32_t n=0;vkEnumeratePhysicalDevices(instance,&n,nullptr);std::vector<VkPhysicalDevice> devices(n);vkEnumeratePhysicalDevices(instance,&n,devices.data());if(!n)return fail("No Vulkan device",VK_ERROR_INITIALIZATION_FAILED);
    physical=devices[0];
    // 启动期间也可能旋转/切后台；等待新窗口，不把瞬时撤回误报为启动失败。
    // Rotation/backgrounding can race startup too; wait for a live window instead of failing boot.
    for(;;){wait_window();r=VkResult(create_surface());if(r==VK_NOT_READY)continue;if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}if(r)return fail("Android surface",r);break;}
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> q(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q.data());bool found=false;
    for(uint32_t i=0;i<n;i++){VkBool32 p=0;vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&p);if(p&&(q[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){family=i;found=true;break;}}if(!found)return fail("No graphics/compute/present family",VK_ERROR_FEATURE_NOT_PRESENT);
    VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&draw;vkGetPhysicalDeviceFeatures2(physical,&features);
    vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr);std::vector<VkExtensionProperties> available(n);vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,available.data());bool mirror=false;for(auto e:available)if(std::string(e.extensionName)==VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)mirror=true;
    {std::string all;for(auto &e:available){all+=e.extensionName;all+=' ';}   // 诊断：设备扩展全表（每次启动一行）| Diagnostics: the full device extension list (one line per launch)
     for(size_t at=0;at<all.size();at+=900)vlog("Device extensions [%zu]: %s",at/900,all.substr(at,900).c_str());
     VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};VkPhysicalDeviceProperties2 pr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};pr.pNext=&host;vkGetPhysicalDeviceProperties2(physical,&pr);
     vlog("Device %s api=%u.%u.%u driver=%#x minImportedHostPointerAlignment=%llu",pr.properties.deviceName,VK_VERSION_MAJOR(pr.properties.apiVersion),VK_VERSION_MINOR(pr.properties.apiVersion),VK_VERSION_PATCH(pr.properties.apiVersion),pr.properties.driverVersion,(unsigned long long)host.minImportedHostPointerAlignment);}
    std::vector<const char*> dext={VK_KHR_SWAPCHAIN_EXTENSION_NAME};if(mirror)dext.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    // Diagnostic (marker gfx-pipe-stats): the driver's per-pipeline statistics (VK_KHR_pipeline_executable_properties).
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    if(FILE *ps=[&]{std::string dir=path;size_t cut=dir.find_last_of('/');return fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-pipe-stats").c_str(),"rb");}()){fclose(ps);dext.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);pef.pipelineExecutableInfo=VK_TRUE;pef.pNext=draw.pNext;draw.pNext=&pef;}
    float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&draw;dc.pEnabledFeatures=&features.features;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.enabledExtensionCount=uint32_t(dext.size());dc.ppEnabledExtensionNames=dext.data();if((r=vkCreateDevice(physical,&dc,nullptr,&device)))return fail("vkCreateDevice",r);vkGetDeviceQueue(device,family,0,&queue);
    for(;;){
        wait_window();
        if(!surface||window_changed()){
            r=VkResult(create_surface());if(r==VK_NOT_READY)continue;
            if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
            if(r)return fail("Android surface",r);
        }
        r=VkResult(create_swap());
        if(r==VK_NOT_READY||r==VK_ERROR_OUT_OF_DATE_KHR){wait_resize();continue;}
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return fail("Swapchain",r);break;
    }
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;if((r=vkCreateCommandPool(device,&cp,nullptr,&command_pool)))return fail("Present command pool",r);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=command_pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;if((r=vkAllocateCommandBuffers(device,&ca,&command)))return fail("Present command",r);
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};if((r=vkCreateSemaphore(device,&sc,nullptr,&acquired)))return fail("Acquire semaphore",r);VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if((r=vkCreateFence(device,&fc,nullptr,&fence)))return fail("Present fence",r);
    {std::string dir=path;size_t cut=dir.find_last_of('/');FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-overlap").c_str(),"rb");overlap=true;if(m){overlap=fgetc(m)!='0';fclose(m);}
     auto open_marker=[&](const char *name){return fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+name).c_str(),"rb");};
     pin_mask=fastest_cores();pin_ab=0;pin_state=-1;pin_have_original=false;pin_presents=pin_drifts=0;
     if(FILE *p=open_marker("/gfx-pin-cpu")){if(fscanf(p,"%x",&pin_mask)!=1)pin_mask=0;fclose(p);}
     if(FILE *n=open_marker("/gfx-no-pin")){pin_mask=0;fclose(n);}
     if(FILE *t=open_marker("/gfx-ab-target")){char target[16]={0};if(fscanf(t,"%15s",target)==1 && !strcmp(target,"pin"))if(FILE *a=open_marker("/gfx-ab-toggle")){if(fscanf(a,"%d",&pin_ab)!=1)pin_ab=0;fclose(a);}fclose(t);}
     vlog("NVN pin mask=%x ab=%d (markers gfx-no-pin, gfx-pin-cpu, gfx-ab-target=pin)",pin_mask,pin_ab);
     if(FILE *b=open_marker("/gfx-membench")){fclose(b);membench();}}
    if(overlap){if((r=vkCreateCommandPool(device,&cp,nullptr,&overlap_pool)))return fail("Present command pool",r);ca.commandPool=overlap_pool;if((r=vkAllocateCommandBuffers(device,&ca,&overlap_command)))return fail("Present command",r);
        if((r=vkCreateSemaphore(device,&sc,nullptr,&overlap_acquired)))return fail("Acquire semaphore",r);if((r=vkCreateFence(device,&fc,nullptr,&overlap_fence)))return fail("Present fence",r);}
    vlog("Present overlap=%d (marker gfx-present-overlap)",int(overlap));
    shader_path=path;dm_vk_config config{};config.size=sizeof(config);config.game.size=sizeof(config.game);config.game.physical=physical;config.game.device=device;config.game.queue=queue;config.game.queue_family=family;config.game.enabled_core=features.features;config.game.enabled_shader_draw_parameters=draw.shaderDrawParameters;config.game.fence_timeout_ns=10000000000ull;config.game.shader_directory=shader_path.c_str();config.game.present=present;config.game.log=log_message;config.game.fatal=fatal;config.sampler_mirror_clamp_to_edge=mirror;config.has_window=has_window;config.display_hz=0; // Query actual display refresh in frontend; do not guess 60 Hz.
    int status=dm_vk_initialize(&config,error,bytes);if(!status)ready=true;else cleanup_initialization();return status;
}
