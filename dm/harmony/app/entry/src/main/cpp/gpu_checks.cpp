// 仅独立GPU探针。每次提交最多5秒；超时保留在飞资源并阻止重入，不无限WaitIdle。
#include "gpu_checks.h"
#include "gpu_assets.h"
#define VK_USE_PLATFORM_OHOS 1
#ifdef XBL_REAL_SHADER
#include "real_clear_spv.h"
#endif
#ifdef XBL_REAL_2D
#include "xbg_vulkan.h"
#include "real_2d_vs.h"
#include "real_2d_ps.h"
#include "original_logo.h"
#include "logo_expected.h"
#endif
#include <cstdio>
#include <unistd.h>
#include <cmath>
#include <ctime>
#include <vulkan/vulkan.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_display_soloist/native_display_soloist.h>
#include <native_window/external_window.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <condition_variable>
#include <chrono>
#include "presentation_policy.h"
#include "runtime_bridge.h"
#include "screen_touch_policy.h"
#include "game_touch.h"

void XblContentRect(float &x0,float &cw);  // game_graphics.cpp (pillarbox)
namespace {
std::atomic<bool> gpuBusy{false};
std::mutex windowMutex;
OHNativeWindow *currentWindow=nullptr;
uint64_t windowWidth=0,windowHeight=0;
uint64_t windowGeneration=0;
bool gameSurfaceForeground=true,gameSurfaceCancelled=false;
std::atomic<bool> waitingForGameSurface{false};
std::condition_variable windowChanged;
std::mutex screenTouchMutex;
XblScreenTouch screenTouch;
void CancelSurfaceTouch(){CancelGameTouch();std::lock_guard<std::mutex> lock(screenTouchMutex);screenTouch.cancel(DispatchGameUiTouch);}
// Pillarboxed game image: surface x -> x inside the image (same width units). Touches that start on the black bars are ignored.
static float ContentX(float x,uint64_t w,bool *inside){float x0=0,cw=1;XblContentRect(x0,cw);if(cw<=0||cw>=0.999f){if(inside)*inside=true;return x;}
    float n=(x/float(w)-x0)/cw;if(inside)*inside=n>=0&&n<=1;n=n<0?0:n>1?1:n;return n*float(w);}
void SurfaceTouch(OH_NativeXComponent *component,void *window){
    OH_NativeXComponent_TouchEvent event{};uint64_t width=0,height=0;
    if(OH_NativeXComponent_GetTouchEvent(component,window,&event)!=0||OH_NativeXComponent_GetXComponentSize(component,window,&width,&height)!=0){CancelSurfaceTouch();return;}
    unsigned phase=3;
    switch(event.type){
        case OH_NATIVEXCOMPONENT_DOWN:phase=0;break;
        case OH_NATIVEXCOMPONENT_MOVE:phase=1;break;
        case OH_NATIVEXCOMPONENT_UP:phase=2;break;
        case OH_NATIVEXCOMPONENT_CANCEL:phase=3;break;
        default:return;
    }
    {bool inside=true;event.x=ContentX(event.x,width,&inside);if(phase==0&&!inside)return;
     for(uint32_t i=0;i<event.numPoints&&i<OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER;++i)event.touchPoints[i].x=ContentX(event.touchPoints[i].x,width,nullptr);}
    std::lock_guard<std::mutex> lock(screenTouchMutex);
    if(!RouteGameTouch(phase,event.id,event.x,event.y,width,height,event.timeStamp)){
        screenTouch.cancelIfActive(DispatchGameUiTouch);
        // A MOVE packet also contains other pressed fingers. Do not replay their
        // DOWNs or invent a missing finger; stable IDs remain owned by the core.
        if(phase==1&&event.numPoints<=OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER)
            for(uint32_t i=0;i<event.numPoints;++i){const auto &p=event.touchPoints[i];
                if(p.id!=event.id&&p.isPressed)RouteGameTouch(1,p.id,p.x,p.y,width,height,event.timeStamp);}
        return;
    }
    screenTouch.event(phase,event.id,event.x,event.y,width,height,DispatchGameUiTouch);
}
// Touches that land on the floating controller overlay's empty area are
// forwarded here with the same routing as XComponent touches (world mapper
// first, then UI mouse). Coordinates are relative to the game surface.
napi_value SendSurfaceTouchImpl(napi_env env,napi_callback_info info){
    size_t n=6;napi_value a[6]{};double v[6]{};
    if(napi_get_cb_info(env,info,&n,a,nullptr,nullptr)!=napi_ok||n!=6){napi_throw_error(env,nullptr,"Invalid surface touch");return nullptr;}
    for(size_t i=0;i<6;++i)if(napi_get_value_double(env,a[i],&v[i])!=napi_ok||!std::isfinite(v[i])){napi_throw_error(env,nullptr,"Invalid surface touch");return nullptr;}
    const unsigned phase=unsigned(v[0]);const int32_t id=int32_t(v[1]);float x=float(v[2]);const float y=float(v[3]);
    const uint64_t w=v[4]>1?uint64_t(v[4]):0,h=v[5]>1?uint64_t(v[5]):0;
    if(w){bool inside=true;x=ContentX(x,w,&inside);if(phase==0&&!inside){napi_value r{};napi_get_undefined(env,&r);return r;}}
    if(phase>3||!w||!h){napi_throw_error(env,nullptr,"Invalid surface touch");return nullptr;}
    timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);const int64_t ns=int64_t(ts.tv_sec)*1000000000LL+ts.tv_nsec;
    {std::lock_guard<std::mutex> lock(screenTouchMutex);
        if(!RouteGameTouch(phase,id,x,y,w,h,ns))screenTouch.cancelIfActive(DispatchGameUiTouch);
        else screenTouch.event(phase,id,x,y,w,h,DispatchGameUiTouch);}
    napi_value result{};napi_get_undefined(env,&result);return result;
}
void SurfaceChanged(OH_NativeXComponent *component,void *window) {
    CancelSurfaceTouch();
    std::lock_guard<std::mutex> lock(windowMutex);
    auto *w=static_cast<OHNativeWindow *>(window);
    const bool identityChanged=currentWindow!=w;
    const uint64_t previousWidth=windowWidth,previousHeight=windowHeight;
    if(currentWindow!=w){
        if(OH_NativeWindow_NativeObjectReference(w)!=0)return;
        if(currentWindow)OH_NativeWindow_NativeObjectUnreference(currentWindow);
        currentWindow=w;
    }
    if(OH_NativeXComponent_GetXComponentSize(component,window,&windowWidth,&windowHeight)!=0)windowWidth=windowHeight=0;
    SetGameTouchSurface(windowWidth,windowHeight);
    // Vote for a high display refresh: at the default 60 Hz a 35-50 ms frame is held to 50 ms (20 fps) and a slower one
    // to 66 ms (15 fps). At 120 Hz frames land on 8.3 ms steps, so a 40 ms frame shows at 41.7 ms. files/display-60hz disables.
    {static OH_NativeXComponent *rated=nullptr;if(rated!=component && access("/data/storage/el2/base/haps/entry/files/display-60hz",F_OK)!=0){OH_NativeXComponent_ExpectedRateRange r{30,120,120};
        const int rc=OH_NativeXComponent_SetExpectedFrameRateRange(component,&r);const int fc=OH_NativeXComponent_RegisterOnFrameCallback(component,[](OH_NativeXComponent*,uint64_t,uint64_t){});
        static OH_DisplaySoloist *soloist=nullptr;int sc=-99,ss=-99;if(!soloist){soloist=OH_DisplaySoloist_Create(false);if(soloist){DisplaySoloist_ExpectedRateRange dr{30,120,120};sc=OH_DisplaySoloist_SetExpectedFrameRateRange(soloist,&dr);ss=OH_DisplaySoloist_Start(soloist,[](long long,long long,void*){},nullptr);}}
        if(FILE *lf=fopen("/data/storage/el2/base/haps/entry/files/display-rate.txt","w")){fprintf(lf,"xcomponent rc=%d onFrame=%d soloist=%p range=%d start=%d\n",rc,fc,(void*)soloist,sc,ss);fclose(lf);}rated=component;}}
    if(XblSurfaceGeometryChanged(identityChanged,previousWidth,previousHeight,windowWidth,windowHeight)){
        ++windowGeneration;windowChanged.notify_all();
    }
}
void SurfaceDestroyed(OH_NativeXComponent *,void *window) {
    CancelSurfaceTouch();
    std::lock_guard<std::mutex> lock(windowMutex);
    if(currentWindow==window){OH_NativeWindow_NativeObjectUnreference(currentWindow);currentWindow=nullptr;windowWidth=windowHeight=0;++windowGeneration;windowChanged.notify_all();}
}
OH_NativeXComponent_Callback surfaceCallbacks{SurfaceChanged,SurfaceChanged,SurfaceDestroyed,SurfaceTouch};
struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkSurfaceKHR surface{};
    OHNativeWindow *window{};
    bool presentationOutstanding=false;
    bool pending = false;
    uint64_t allocated = 0;
    std::ostringstream out;
    std::vector<std::function<void()>> cleanup;
    VkPhysicalDeviceFeatures enabledCore{};
    VkBool32 enabledDrawParameters=VK_FALSE;
    void Check(VkResult r, const char *label) {
        out << label << '=' << r << '\n';
        if (r != VK_SUCCESS) throw std::runtime_error(label);
    }
    void Require(bool yes, const char *label) {
        out << label << '=' << (yes ? "PASS" : "FAIL") << '\n';
        if (!yes) throw std::runtime_error(label);
    }
    ~Context() {
        // 只有确认无在飞提交（或device lost）才析构；timeout由外层保留整个Context。
        for (auto i = cleanup.rbegin(); i != cleanup.rend(); ++i) (*i)();
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (surface) vkDestroySurfaceKHR(instance,surface,nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (window) OH_NativeWindow_NativeObjectUnreference(window);
    }
    void Init(OHNativeWindow *nativeWindow=nullptr,bool realShader=false,bool real2D=false) {
        window=nativeWindow; // 接收Acquire阶段持有的独立NativeWindow引用。
        VkApplicationInfo app{}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName="XblGpuProbe"; app.apiVersion=(realShader||real2D)?VK_API_VERSION_1_1:VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo=&app;
        const char *instanceExt[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_OHOS_SURFACE_EXTENSION_NAME};
        if(window){ci.enabledExtensionCount=2;ci.ppEnabledExtensionNames=instanceExt;}
        Check(vkCreateInstance(&ci,nullptr,&instance),"createInstance");
        if(window){
            auto createSurface=reinterpret_cast<PFN_vkCreateSurfaceOHOS>(vkGetInstanceProcAddr(instance,"vkCreateSurfaceOHOS"));
            Require(createSurface!=nullptr,"ohosSurfaceFunction");
            VkSurfaceCreateInfoOHOS sci{};sci.sType=VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;sci.window=window;
            Check(createSurface(instance,&sci,nullptr,&surface),"createSurfaceOHOS");
        }
        std::array<VkPhysicalDevice,8> devices{}; uint32_t count=devices.size();
        Check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerateDevices");
        Require(count>0,"deviceAvailable"); physical=devices[0];
        vkGetPhysicalDeviceProperties(physical,&properties);
        vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        VkPhysicalDeviceFeatures features{}; vkGetPhysicalDeviceFeatures(physical,&features);
        VkPhysicalDeviceFeatures enabled{};
        VkPhysicalDeviceShaderDrawParametersFeatures drawFeatures{};drawFeatures.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
#ifdef XBL_REAL_2D
        if(real2D){
            xbg_vk_requirements req{};char error[512]{};
            int result=xbg_vk_query_2d(physical,xbl_real_2d_vs,sizeof(xbl_real_2d_vs),xbl_real_2d_ps,sizeof(xbl_real_2d_ps),&req,error,sizeof(error));
            out << "queryReal2D=" << result << " error=" << error << " subgroupSize=" << req.subgroup.subgroupSize << " stages=" << req.subgroup.supportedStages << " operations=" << req.subgroup.supportedOperations << '\n';
            Require(result==0,"real2DRequirements");enabled=req.core;drawFeatures.shaderDrawParameters=req.shader_draw_parameters;
            out << "enabled2D shaderImageGatherExtended=" << enabled.shaderImageGatherExtended << " shaderDrawParameters=" << drawFeatures.shaderDrawParameters << '\n';
        }
#endif
        if(realShader){
            Require(properties.apiVersion>=VK_API_VERSION_1_1,"realShaderVulkan11");
            VkPhysicalDeviceSubgroupProperties subgroup{};subgroup.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
            VkPhysicalDeviceProperties2 p2{};p2.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;p2.pNext=&subgroup;
            vkGetPhysicalDeviceProperties2(physical,&p2);
            out << "subgroup size=" << subgroup.subgroupSize << " stages=" << subgroup.supportedStages << " operations=" << subgroup.supportedOperations << '\n';
            const auto ops=VK_SUBGROUP_FEATURE_BASIC_BIT|VK_SUBGROUP_FEATURE_BALLOT_BIT|VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
            Require((subgroup.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT) && (subgroup.supportedOperations&ops)==ops,"realShaderSubgroupCapabilities64_65");
            out << "shaderImageGatherExtended=" << features.shaderImageGatherExtended << '\n';
            Require(features.shaderImageGatherExtended,"realShaderCapability25");
            enabled.shaderImageGatherExtended=VK_TRUE;
            out << "enabled shaderImageGatherExtended=1; capabilities1_46_47_50=core; SPIRV=1.3\n";
        }
        out << "gpu=" << properties.deviceName << " driver=" << properties.driverVersion
            << " api=" << properties.apiVersion << " tessellationShader=" << features.tessellationShader
            << " shaderInt64=" << features.shaderInt64 << '\n';
        const auto &l=properties.limits;
        out << "limits maxBoundDescriptorSets=" << l.maxBoundDescriptorSets
            << " maxPerStageDescriptorUniformBuffers=" << l.maxPerStageDescriptorUniformBuffers
            << " maxPerStageDescriptorStorageBuffers=" << l.maxPerStageDescriptorStorageBuffers
            << " maxPerStageDescriptorSamplers=" << l.maxPerStageDescriptorSamplers
            << " maxPerStageDescriptorSampledImages=" << l.maxPerStageDescriptorSampledImages
            << " maxDescriptorSetUniformBuffers=" << l.maxDescriptorSetUniformBuffers
            << " maxDescriptorSetStorageBuffers=" << l.maxDescriptorSetStorageBuffers
            << " maxUniformBufferRange=" << l.maxUniformBufferRange
            << " maxStorageBufferRange=" << l.maxStorageBufferRange
            << " minUniformBufferOffsetAlignment=" << l.minUniformBufferOffsetAlignment
            << " minStorageBufferOffsetAlignment=" << l.minStorageBufferOffsetAlignment
            << " maxComputeSharedMemorySize=" << l.maxComputeSharedMemorySize
            << " maxComputeWorkGroupInvocations=" << l.maxComputeWorkGroupInvocations
            << " maxComputeWorkGroupSize=" << l.maxComputeWorkGroupSize[0] << ',' << l.maxComputeWorkGroupSize[1] << ',' << l.maxComputeWorkGroupSize[2]
            << " maxComputeWorkGroupCount=" << l.maxComputeWorkGroupCount[0] << ',' << l.maxComputeWorkGroupCount[1] << ',' << l.maxComputeWorkGroupCount[2]
            << " nonCoherentAtomSize=" << l.nonCoherentAtomSize << '\n';
        Require(l.maxComputeWorkGroupSize[0]>=32 && l.maxComputeWorkGroupSize[1]>=1 && l.maxComputeWorkGroupSize[2]>=1 &&
                l.maxComputeWorkGroupInvocations>=32 && l.maxComputeWorkGroupCount[0]>=2 &&
                l.maxComputeWorkGroupCount[1]>=1 && l.maxComputeWorkGroupCount[2]>=1 && l.maxUniformBufferRange>=16 &&
                l.maxStorageBufferRange>=1280 && l.maxPerStageDescriptorStorageBuffers>=2,"computeLimits");
        uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,nullptr);
        Require(nq>0 && nq<=64,"queueFamilyBound");
        std::vector<VkQueueFamilyProperties> qs(nq); vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,qs.data());
        family=UINT32_MAX;
        for(uint32_t i=0;i<nq;++i) if(qs[i].queueCount && (qs[i].queueFlags & (VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)) == (VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)) {
            VkBool32 supported=VK_TRUE;if(surface)Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&supported),"surfaceQueueSupport");
            if(supported){family=i;break;}
        }
        Require(family!=UINT32_MAX,"graphicsComputeQueue"); out << "queueFamily=" << family << " flags=" << qs[family].queueFlags << '\n';
        float priority=1;
        VkDeviceQueueCreateInfo qi{}; qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo di{}; di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
        if(realShader||real2D)di.pEnabledFeatures=&enabled;
        if(real2D)di.pNext=&drawFeatures;
        enabledCore=enabled;enabledDrawParameters=drawFeatures.shaderDrawParameters;
        const char *deviceExt[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if(surface){di.enabledExtensionCount=1;di.ppEnabledExtensionNames=deviceExt;}
        // 不启用未使用的Int64/tessellation特性；读数不代表pipeline已用这些能力。
        Check(vkCreateDevice(physical,&di,nullptr,&device),"createDevice"); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pi{}; pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex=family; pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        Check(vkCreateCommandPool(device,&pi,nullptr,&pool),"createCommandPool");
        VkCommandBufferAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
        Check(vkAllocateCommandBuffers(device,&ai,&command),"allocateCommandBuffer");
        VkFenceCreateInfo fi{}; fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        Check(vkCreateFence(device,&fi,nullptr,&fence),"createFence");
    }
    uint32_t MemoryType(uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred=0) {
        for(unsigned pass=0;pass<2;++pass) for(uint32_t i=0;i<memory.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&required)==required &&
                (pass || (memory.memoryTypes[i].propertyFlags&preferred)==preferred)) return i;
        throw std::runtime_error("No compatible memory type");
    }
    VkDeviceMemory Allocate(const VkMemoryRequirements &req,uint32_t type) {
        Require(req.size<=8*1024*1024 && allocated+req.size<=16*1024*1024,"allocationBudget");
        VkMemoryAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
        VkDeviceMemory m{}; Check(vkAllocateMemory(device,&ai,nullptr,&m),"allocateMemory"); allocated+=req.size;
        cleanup.push_back([this,m]{vkFreeMemory(device,m,nullptr);});
        out << "allocation bytes=" << req.size << " alignment=" << req.alignment << " memoryType=" << type << " flags=" << memory.memoryTypes[type].propertyFlags << '\n';
        return m;
    }
    void Begin() {
        Check(vkResetFences(device,1,&fence),"resetFence"); Check(vkResetCommandBuffer(command,0),"resetCommandBuffer");
        VkCommandBufferBeginInfo bi{}; bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command,&bi),"beginCommandBuffer");
    }
    void Submit(VkSemaphore signal=VK_NULL_HANDLE) {
        Check(vkEndCommandBuffer(command),"endCommandBuffer");
        VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&command;
        if(signal){si.signalSemaphoreCount=1;si.pSignalSemaphores=&signal;}
        Check(vkQueueSubmit(queue,1,&si,fence),"queueSubmit"); pending=true;
        VkResult r=vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ULL);
        if(r==VK_SUCCESS || r==VK_ERROR_DEVICE_LOST) pending=false;
        Check(r,"fenceWait5s");
    }
};
// 超时只保留一个上下文，直到应用进程结束；绝不释放在飞资源或继续分配。
Context *quarantined=nullptr;
Context *presented=nullptr;
struct Buffer {VkBuffer buffer{}; VkDeviceMemory memory{}; void *mapped{}; VkDeviceSize size{}; bool coherent{};};
Buffer MakeBuffer(Context &c,VkDeviceSize size,VkBufferUsageFlags usage) {
    Buffer b{}; b.size=size;
    VkBufferCreateInfo bi{}; bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size=size; bi.usage=usage; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    c.Check(vkCreateBuffer(c.device,&bi,nullptr,&b.buffer),"createBuffer");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(c.device,b.buffer,&req);
    try {
        uint32_t type=c.MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        b.coherent=(c.memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        b.memory=c.Allocate(req,type);
    } catch(...) {vkDestroyBuffer(c.device,b.buffer,nullptr);throw;}
    c.cleanup.push_back([&c,handle=b.buffer]{vkDestroyBuffer(c.device,handle,nullptr);});
    c.Check(vkBindBufferMemory(c.device,b.buffer,b.memory,0),"bindBufferMemory");
    c.Check(vkMapMemory(c.device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped),"mapMemory");
    c.cleanup.push_back([&c,m=b.memory]{vkUnmapMemory(c.device,m);});
    return b;
}
void Cache(Context &c,const Buffer &b,bool invalidate) {
    c.out << (invalidate?"invalidate":"flush") << " coherent=" << b.coherent << '\n';
    if(b.coherent) return;
    VkMappedMemoryRange r{}; r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; r.memory=b.memory; r.offset=0; r.size=VK_WHOLE_SIZE;
    c.Check(invalidate?vkInvalidateMappedMemoryRanges(c.device,1,&r):vkFlushMappedMemoryRanges(c.device,1,&r),invalidate?"invalidateMemory":"flushMemory");
}
void BufferBarrier(Context &c,const Buffer &b,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags from,VkPipelineStageFlags to) {
    VkBufferMemoryBarrier bar{}; bar.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; bar.srcAccessMask=src;bar.dstAccessMask=dst;
    bar.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; bar.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; bar.buffer=b.buffer;bar.size=b.size;
    vkCmdPipelineBarrier(c.command,from,to,0,0,nullptr,1,&bar,0,nullptr);
}
VkShaderModule Shader(Context &c,const uint32_t *data,size_t bytes) {
    VkShaderModuleCreateInfo mi{}; mi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;mi.codeSize=bytes;mi.pCode=data;
    VkShaderModule m{}; c.Check(vkCreateShaderModule(c.device,&mi,nullptr,&m),"createShaderModule");
    c.cleanup.push_back([&c,m]{vkDestroyShaderModule(c.device,m,nullptr);});return m;
}
std::string Hex(const void *data,size_t size) {
    const auto *b=static_cast<const uint8_t *>(data); std::string s; s.reserve(size*2);
    constexpr char digits[]="0123456789abcdef";
    for(size_t i=0;i<size;++i){s+=digits[b[i]>>4];s+=digits[b[i]&15];} return s;
}
void Compute(Context &c) {
    using namespace gpu_assets;
    const uint32_t *code[]={q_val0_spv,q_val8_spv,q_dlt0_spv,q_dlt8_spv};
    const size_t bytes[]={sizeof(q_val0_spv),sizeof(q_val8_spv),sizeof(q_dlt0_spv),sizeof(q_dlt8_spv)};
    const char *hashes[]={q_val0_spv_sha256,q_val8_spv_sha256,q_dlt0_spv_sha256,q_dlt8_spv_sha256};
    const char *names[]={"q_val0","q_val8","q_dlt0","q_dlt8"};
    const uint32_t *expected[]={q_val0_expected_bin,q_val8_expected_bin,q_dlt0_expected_bin,q_dlt8_expected_bin};
    Buffer ubo=MakeBuffer(c,16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    Buffer src=MakeBuffer(c,1280,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer dst=MakeBuffer(c,512,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(ubo.mapped,count_ubo_bin,16); memcpy(src.mapped,source_ssbo_bin,1280);Cache(c,ubo,false);Cache(c,src,false);
    std::array<VkDescriptorSetLayoutBinding,3> bindings{};
    for(uint32_t i=0;i<3;++i){bindings[i].binding=i;bindings[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;li.bindingCount=3;li.pBindings=bindings.data();
    VkDescriptorSetLayout layout{};c.Check(vkCreateDescriptorSetLayout(c.device,&li,nullptr,&layout),"createComputeSetLayout");
    c.cleanup.push_back([&c,layout]{vkDestroyDescriptorSetLayout(c.device,layout,nullptr);});
    VkPipelineLayoutCreateInfo pli{};pli.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pli.setLayoutCount=1;pli.pSetLayouts=&layout;
    VkPipelineLayout pl{};c.Check(vkCreatePipelineLayout(c.device,&pli,nullptr,&pl),"createComputePipelineLayout");c.cleanup.push_back([&c,pl]{vkDestroyPipelineLayout(c.device,pl,nullptr);});
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    VkDescriptorPoolCreateInfo dpi{};dpi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dpi.maxSets=1;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes;
    VkDescriptorPool dp{};c.Check(vkCreateDescriptorPool(c.device,&dpi,nullptr,&dp),"createComputeDescriptorPool");c.cleanup.push_back([&c,dp]{vkDestroyDescriptorPool(c.device,dp,nullptr);});
    VkDescriptorSetAllocateInfo dai{};dai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;dai.descriptorPool=dp;dai.descriptorSetCount=1;dai.pSetLayouts=&layout;
    VkDescriptorSet set{};c.Check(vkAllocateDescriptorSets(c.device,&dai,&set),"allocateComputeSet");
    VkDescriptorBufferInfo infos[]={{ubo.buffer,0,16},{src.buffer,0,1280},{dst.buffer,0,512}};
    std::array<VkWriteDescriptorSet,3> writes{};
    for(uint32_t i=0;i<3;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pBufferInfo=&infos[i];}
    vkUpdateDescriptorSets(c.device,3,writes.data(),0,nullptr);
    bool all=true;
    for(unsigned k=0;k<4;++k){
        c.out << "kernel=" << names[k] << " shaderSha256=" << hashes[k] << '\n';
        VkShaderModule sm=Shader(c,code[k],bytes[k]);
        VkComputePipelineCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;pi.layout=pl;
        pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=sm;pi.stage.pName="main";
        VkPipeline pipeline{};c.Check(vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline),"createComputePipeline");c.cleanup.push_back([&c,pipeline]{vkDestroyPipeline(c.device,pipeline,nullptr);});
        memcpy(dst.mapped,destination_sentinel_bin,512);Cache(c,dst,false);c.Begin();
        BufferBarrier(c,ubo,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_UNIFORM_READ_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        BufferBarrier(c,src,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        BufferBarrier(c,dst,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,nullptr);
        vkCmdDispatch(c.command,2,1,1);
        BufferBarrier(c,dst,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
        c.Submit();Cache(c,dst,true);
        c.out << "matrix " << names[k] << '=';
        for(unsigned e=0;e<4;++e){bool equal=memcmp(dst.mapped,expected[e],512)==0;c.out << (equal?1:0);all=all&&(equal==(e==k));}
        auto *data=static_cast<const uint8_t *>(dst.mapped);size_t mismatch=512;
        for(size_t b=0;b<512;++b) if(data[b]!=reinterpret_cast<const uint8_t *>(expected[k])[b]){mismatch=b;break;}
        bool sentinel=true;for(size_t b=296;b<512;++b) sentinel=sentinel&&data[b]==0xaa;
        c.out << " firstMismatch=" << (mismatch==512?-1:static_cast<int>(mismatch)) << " sentinel=" << sentinel << '\n';all=all&&sentinel;
        c.out << "readbackHex " << names[k] << '=' << Hex(dst.mapped,512) << '\n';
    }
    c.Require(all,"fourKernelDispatchReadback");c.out << "computeAllocatedBytes=" << c.allocated << '\n';
}
struct Image {VkImage image{};VkImageView view{};};
Image MakeImage(Context &c,uint32_t w,uint32_t h,VkFormat format,VkImageUsageFlags usage) {
    Image im{};
    VkImageCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;
    ci.extent={w,h,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    c.Check(vkCreateImage(c.device,&ci,nullptr,&im.image),"createImage");
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(c.device,im.image,&req);
    VkDeviceMemory mem{};
    try {mem=c.Allocate(req,c.MemoryType(req.memoryTypeBits,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));}
    catch(...){vkDestroyImage(c.device,im.image,nullptr);throw;}
    c.cleanup.push_back([&c,i=im.image]{vkDestroyImage(c.device,i,nullptr);});
    c.Check(vkBindImageMemory(c.device,im.image,mem,0),"bindImageMemory");
    VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=im.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    c.Check(vkCreateImageView(c.device,&vi,nullptr,&im.view),"createImageView");c.cleanup.push_back([&c,v=im.view]{vkDestroyImageView(c.device,v,nullptr);});
    return im;
}
void ImageBarrier(Context &c,VkImage image,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags from,VkPipelineStageFlags to) {
    VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.oldLayout=oldLayout;b.newLayout=newLayout;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(c.command,from,to,0,0,nullptr,0,nullptr,1,&b);
}
constexpr uint8_t testPixels[64]={
    255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255,
    255,255,0,255, 255,0,255,255, 0,255,255,255, 0,0,0,255,
    255,0,0,85, 0,255,0,170, 0,0,255,0, 255,255,255,255,
    255,128,64,255, 64,128,255,255, 128,64,255,255, 32,64,128,255
};
struct DrawPass {VkRenderPass renderPass{};VkFramebuffer framebuffer{};VkPipeline pipeline{};VkPipelineLayout layout{};VkDescriptorSet set{};};
DrawPass MakeDraw(Context &c,VkFormat format,VkImageView target,uint32_t w,uint32_t h,VkImageView texture) {
    DrawPass d{};
    VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;si.magFilter=VK_FILTER_NEAREST;si.minFilter=VK_FILTER_NEAREST;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;
    VkSampler sampler{};c.Check(vkCreateSampler(c.device,&si,nullptr,&sampler),"createSampler");c.cleanup.push_back([&c,sampler]{vkDestroySampler(c.device,sampler,nullptr);});
    VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorCount=1;binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;li.bindingCount=1;li.pBindings=&binding;
    VkDescriptorSetLayout sl{};c.Check(vkCreateDescriptorSetLayout(c.device,&li,nullptr,&sl),"createSampleSetLayout");c.cleanup.push_back([&c,sl]{vkDestroyDescriptorSetLayout(c.device,sl,nullptr);});
    VkPipelineLayoutCreateInfo pli{};pli.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pli.setLayoutCount=1;pli.pSetLayouts=&sl;
    c.Check(vkCreatePipelineLayout(c.device,&pli,nullptr,&d.layout),"createSamplePipelineLayout");c.cleanup.push_back([&c,l=d.layout]{vkDestroyPipelineLayout(c.device,l,nullptr);});
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};VkDescriptorPoolCreateInfo dpi{};dpi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dpi.maxSets=1;dpi.poolSizeCount=1;dpi.pPoolSizes=&ps;
    VkDescriptorPool dp{};c.Check(vkCreateDescriptorPool(c.device,&dpi,nullptr,&dp),"createSamplePool");c.cleanup.push_back([&c,dp]{vkDestroyDescriptorPool(c.device,dp,nullptr);});
    VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=dp;ai.descriptorSetCount=1;ai.pSetLayouts=&sl;
    c.Check(vkAllocateDescriptorSets(c.device,&ai,&d.set),"allocateSampleSet");
    VkDescriptorImageInfo ii{sampler,texture,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet wr{};wr.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr.dstSet=d.set;wr.dstBinding=0;wr.descriptorCount=1;wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&ii;
    vkUpdateDescriptorSets(c.device,1,&wr,0,nullptr);
    VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
    VkRenderPassCreateInfo ri{};ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;ri.attachmentCount=1;ri.pAttachments=&attachment;ri.subpassCount=1;ri.pSubpasses=&sub;
    c.Check(vkCreateRenderPass(c.device,&ri,nullptr,&d.renderPass),"createRenderPass");c.cleanup.push_back([&c,r=d.renderPass]{vkDestroyRenderPass(c.device,r,nullptr);});
    VkFramebufferCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fi.renderPass=d.renderPass;fi.attachmentCount=1;fi.pAttachments=&target;fi.width=w;fi.height=h;fi.layers=1;
    c.Check(vkCreateFramebuffer(c.device,&fi,nullptr,&d.framebuffer),"createFramebuffer");c.cleanup.push_back([&c,f=d.framebuffer]{vkDestroyFramebuffer(c.device,f,nullptr);});
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=Shader(c,gpu_assets::sample_vert_spv,sizeof(gpu_assets::sample_vert_spv));stages[0].pName="main";
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=Shader(c,gpu_assets::sample_frag_spv,sizeof(gpu_assets::sample_frag_spv));stages[1].pName="main";
    VkPipelineVertexInputStateCreateInfo vertex{};vertex.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,static_cast<float>(w),static_cast<float>(h),0,1};VkRect2D scissor{{0,0},{w,h}};
    VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;blend.attachmentCount=1;blend.pAttachments=&ba;
    VkGraphicsPipelineCreateInfo gp{};gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gp.stageCount=2;gp.pStages=stages;gp.pVertexInputState=&vertex;gp.pInputAssemblyState=&assembly;gp.pViewportState=&vp;gp.pRasterizationState=&raster;gp.pMultisampleState=&ms;gp.pColorBlendState=&blend;gp.layout=d.layout;gp.renderPass=d.renderPass;
    c.Check(vkCreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&gp,nullptr,&d.pipeline),"createGraphicsPipeline");c.cleanup.push_back([&c,p=d.pipeline]{vkDestroyPipeline(c.device,p,nullptr);});
    return d;
}
void UploadTexture(Context &c,const Buffer &staging,const Image &texture) {
    BufferBarrier(c,staging,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    ImageBarrier(c,texture.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={4,4,1};
    vkCmdCopyBufferToImage(c.command,staging.buffer,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    ImageBarrier(c,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}
void RecordDraw(Context &c,const DrawPass &d,uint32_t w,uint32_t h) {
    VkClearValue clear{};clear.color.float32[3]=1;
    VkRenderPassBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;bi.renderPass=d.renderPass;bi.framebuffer=d.framebuffer;bi.renderArea={{0,0},{w,h}};bi.clearValueCount=1;bi.pClearValues=&clear;
    vkCmdBeginRenderPass(c.command,&bi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,d.pipeline);
    vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,d.layout,0,1,&d.set,0,nullptr);vkCmdDraw(c.command,3,1,0,0);vkCmdEndRenderPass(c.command);
}
void SampleReadback(Context &c) {
    constexpr uint32_t width=32,height=32;
    VkFormatProperties fmt{};vkGetPhysicalDeviceFormatProperties(c.physical,VK_FORMAT_R8G8B8A8_UNORM,&fmt);
    c.out << "rgba8OptimalFeatures=" << fmt.optimalTilingFeatures << '\n';
    const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    c.Require((fmt.optimalTilingFeatures&required)==required,"rgba8SampleAndColorSupport");
    // vkGetPhysicalDeviceImageFormatProperties验证具体usage组合，包含transfer能力。
    VkImageFormatProperties ip{};
    c.Check(vkGetPhysicalDeviceImageFormatProperties(c.physical,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,0,&ip),"sampleImageUsageSupport");
    c.Check(vkGetPhysicalDeviceImageFormatProperties(c.physical,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,0,&ip),"targetImageUsageSupport");
    Buffer upload=MakeBuffer(c,sizeof(testPixels),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);memcpy(upload.mapped,testPixels,sizeof(testPixels));Cache(c,upload,false);
    Buffer read=MakeBuffer(c,width*height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);memset(read.mapped,0xaa,width*height*4);Cache(c,read,false);
    Image texture=MakeImage(c,4,4,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    Image target=MakeImage(c,width,height,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    DrawPass d=MakeDraw(c,VK_FORMAT_R8G8B8A8_UNORM,target.view,width,height,texture.view);
    c.Begin();UploadTexture(c,upload,texture);
    ImageBarrier(c,target.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    RecordDraw(c,d,width,height);
    ImageBarrier(c,target.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    BufferBarrier(c,read,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(c.command,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,read.buffer,1,&copy);
    BufferBarrier(c,read,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);c.Submit();Cache(c,read,true);
    const auto *actual=static_cast<const uint8_t *>(read.mapped);uint32_t differences=0;int first=-1;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)for(uint32_t ch=0;ch<4;++ch){uint32_t at=(y*width+x)*4+ch;uint8_t expected=testPixels[((y/8)*4+x/8)*4+ch];if(actual[at]!=expected){++differences;if(first<0)first=at;}}
    c.out << "rgbaReadback width=32 height=32 channels=4096 mismatch=" << differences << " firstMismatch=" << first << '\n';
    c.out << "readbackHex rgba32=" << Hex(read.mapped,width*height*4) << '\n';
    c.Require(differences==0,"textureUploadSampleDrawReadback");
    c.out << "totalAllocatedBytes=" << c.allocated << " textureFormat=RGBA8_UNORM filter=NEAREST blending=OFF\n";
}
void Present(Context &c,uint64_t requestedWidth,uint64_t requestedHeight,VkImage source=VK_NULL_HANDLE,uint32_t sourceWidth=0,uint32_t sourceHeight=0) {
    VkSurfaceCapabilitiesKHR caps{};c.Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.physical,c.surface,&caps),"surfaceCapabilities");
    std::array<VkSurfaceFormatKHR,64> formats{};uint32_t nf=formats.size();
    c.Check(vkGetPhysicalDeviceSurfaceFormatsKHR(c.physical,c.surface,&nf,formats.data()),"surfaceFormats");
    VkSurfaceFormatKHR format{};bool found=false;
    for(uint32_t i=0;i<nf;++i)if((formats[i].format==VK_FORMAT_R8G8B8A8_UNORM || formats[i].format==VK_FORMAT_B8G8R8A8_UNORM) && formats[i].colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR){format=formats[i];found=true;break;}
    c.Require(found,"surfaceUnormFormat");
    VkExtent2D extent=caps.currentExtent;
    if(extent.width==UINT32_MAX){extent.width=std::clamp(static_cast<uint32_t>(requestedWidth),caps.minImageExtent.width,caps.maxImageExtent.width);extent.height=std::clamp(static_cast<uint32_t>(requestedHeight),caps.minImageExtent.height,caps.maxImageExtent.height);}
    uint32_t imageCount=caps.minImageCount+1;if(caps.maxImageCount && imageCount>caps.maxImageCount)imageCount=caps.maxImageCount;
    c.Require(imageCount<=4 && extent.width>0 && extent.height>0 && extent.width<=2048 && extent.height<=2048 &&
        uint64_t(extent.width)*extent.height*4*imageCount<=32*1024*1024,"swapchainSizeBudget");
    VkImageUsageFlags usage=source?VK_IMAGE_USAGE_TRANSFER_DST_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    c.Require((caps.supportedUsageFlags&usage)!=0,"swapchainRequiredUsage");
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(caps.supportedCompositeAlpha&alpha)) alpha=static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));
    VkSwapchainCreateInfoKHR si{};si.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;si.surface=c.surface;si.minImageCount=imageCount;si.imageFormat=format.format;si.imageColorSpace=format.colorSpace;si.imageExtent=extent;
    si.imageArrayLayers=1;si.imageUsage=usage;si.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;si.preTransform=caps.currentTransform;si.compositeAlpha=alpha;si.presentMode=VK_PRESENT_MODE_FIFO_KHR;si.clipped=VK_TRUE;
    VkSwapchainKHR swap{};c.Check(vkCreateSwapchainKHR(c.device,&si,nullptr,&swap),"createSwapchain");c.cleanup.push_back([&c,swap]{vkDestroySwapchainKHR(c.device,swap,nullptr);});
    std::array<VkImage,8> images{};uint32_t ni=images.size();c.Check(vkGetSwapchainImagesKHR(c.device,swap,&ni,images.data()),"swapchainImages");
    c.Require(ni<=4 && uint64_t(extent.width)*extent.height*4*ni<=32*1024*1024,"actualSwapchainSizeBudget");
    c.out << "surface width=" << extent.width << " height=" << extent.height << " format=" << format.format << " imageCount=" << ni << " imageBytesLowerBound=" << uint64_t(extent.width)*extent.height*4*ni << '\n';
    // 用acquire fence在CPU有界等待图像就绪，不依赖隐式acquire完成。
    VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;VkFence acquired{};
    c.Check(vkCreateFence(c.device,&fi,nullptr,&acquired),"createAcquireFence");c.cleanup.push_back([&c,acquired]{vkDestroyFence(c.device,acquired,nullptr);});
    uint32_t index=0;VkResult ar=vkAcquireNextImageKHR(c.device,swap,1000000000ULL,VK_NULL_HANDLE,acquired,&index);
    c.out << "acquireNextImage1s=" << ar << '\n';if(ar!=VK_SUCCESS && ar!=VK_SUBOPTIMAL_KHR)throw std::runtime_error("acquireNextImage");
    c.pending=true;VkResult wait=vkWaitForFences(c.device,1,&acquired,VK_TRUE,5000000000ULL);
    if(wait==VK_SUCCESS||wait==VK_ERROR_DEVICE_LOST)c.pending=false;c.Check(wait,"acquireFenceWait5s");
    c.Require(index<ni,"acquiredIndex");
    VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=images[index];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView view{};if(!source){c.Check(vkCreateImageView(c.device,&vi,nullptr,&view),"createSwapchainView");c.cleanup.push_back([&c,view]{vkDestroyImageView(c.device,view,nullptr);});}
    Buffer upload{};Image texture{};DrawPass d{};
    if(!source){
        upload=MakeBuffer(c,sizeof(testPixels),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);memcpy(upload.mapped,testPixels,sizeof(testPixels));Cache(c,upload,false);
        texture=MakeImage(c,4,4,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        d=MakeDraw(c,format.format,view,extent.width,extent.height,texture.view);
    }
    VkSemaphoreCreateInfo sem{};sem.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;VkSemaphore rendered{};
    c.Check(vkCreateSemaphore(c.device,&sem,nullptr,&rendered),"createRenderSemaphore");c.cleanup.push_back([&c,rendered]{vkDestroySemaphore(c.device,rendered,nullptr);});
    c.Begin();
    if(source){
        VkFormatProperties src{},dst{};vkGetPhysicalDeviceFormatProperties(c.physical,VK_FORMAT_R8G8B8A8_UNORM,&src);vkGetPhysicalDeviceFormatProperties(c.physical,format.format,&dst);
        c.Require((src.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)&&(dst.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT),"real2DBlitFormatSupport");
        ImageBarrier(c,images[index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;blit.srcOffsets[1]={static_cast<int32_t>(sourceWidth),static_cast<int32_t>(sourceHeight),1};blit.dstOffsets[1]={static_cast<int32_t>(extent.width),static_cast<int32_t>(extent.height),1};
        vkCmdBlitImage(c.command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        ImageBarrier(c,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }else{
    UploadTexture(c,upload,texture);
    ImageBarrier(c,images[index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    RecordDraw(c,d,extent.width,extent.height);
    ImageBarrier(c,images[index],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    c.Submit(rendered);
    VkPresentInfoKHR pi{};pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&rendered;pi.swapchainCount=1;pi.pSwapchains=&swap;pi.pImageIndices=&index;
    // 图形fence不证明presentation已消费完资源。单帧probe保留整个上下文，直到进程退出。
    // 每进程最多一个；不使用无界QueueWaitIdle，也不在surface销毁回调强拆GPU对象。
    c.presentationOutstanding=true;VkResult pr=vkQueuePresentKHR(c.queue,&pi);c.out << "queuePresent=" << pr << '\n';
    if(pr!=VK_SUCCESS && pr!=VK_SUBOPTIMAL_KHR)throw std::runtime_error("queuePresent");
    c.out << "windowPresent=SUBMITTED frames=1 screenshot_required context=RETAINED_UNTIL_PROCESS_EXIT game=NOT_INTEGRATED\n";
}
void RealClear(Context &c) {
#ifndef XBL_REAL_SHADER
    c.Require(false,"realShaderExternalAssetNotConfigured");
#else
    c.out << "realShader key=6ec4bfde22385501 name=cs_assao_clear_load_counter_c sha256=b459300985dc6fb3ca6b1878cdf2f256b87d976012b06816271a4e75608866e6 bytes=" << sizeof(xbl_real_clear_spv) << '\n';
    c.Require(sizeof(xbl_real_clear_spv)==896,"realShaderSize");
    c.Require(c.properties.limits.maxUniformBufferRange>=1392 && c.properties.limits.maxPerStageDescriptorUniformBuffers>=1 && c.properties.limits.maxPerStageDescriptorStorageBuffers>=1,"realShaderDescriptorLimits");
    Buffer support=MakeBuffer(c,1392,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    Buffer output=MakeBuffer(c,16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(support.mapped,0,1392);memset(output.mapped,0xff,16);Cache(c,support,false);Cache(c,output,false);
    c.out << "realInput=" << Hex(output.mapped,16) << " supportBytes=1392 support=ZERO_UNUSED binding5=UBO binding11=SSBO nvnSlot=3 c0ByteOffset=832\n";
    VkDescriptorSetLayoutBinding bindings[2]{};
    for(unsigned i=0;i<2;++i){bindings[i].binding=i?11:5;bindings[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;li.bindingCount=2;li.pBindings=bindings;
    VkDescriptorSetLayout layout{};c.Check(vkCreateDescriptorSetLayout(c.device,&li,nullptr,&layout),"realSetLayout");c.cleanup.push_back([&c,layout]{vkDestroyDescriptorSetLayout(c.device,layout,nullptr);});
    VkPipelineLayoutCreateInfo pli{};pli.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pli.setLayoutCount=1;pli.pSetLayouts=&layout;
    VkPipelineLayout pl{};c.Check(vkCreatePipelineLayout(c.device,&pli,nullptr,&pl),"realPipelineLayout");c.cleanup.push_back([&c,pl]{vkDestroyPipelineLayout(c.device,pl,nullptr);});
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dpi{};dpi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dpi.maxSets=1;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes;
    VkDescriptorPool pool{};c.Check(vkCreateDescriptorPool(c.device,&dpi,nullptr,&pool),"realDescriptorPool");c.cleanup.push_back([&c,pool]{vkDestroyDescriptorPool(c.device,pool,nullptr);});
    VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
    VkDescriptorSet set{};c.Check(vkAllocateDescriptorSets(c.device,&ai,&set),"realDescriptorSet");
    VkDescriptorBufferInfo infos[]={{support.buffer,0,1392},{output.buffer,0,16}};VkWriteDescriptorSet wr[2]{};
    for(unsigned i=0;i<2;++i){wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=set;wr[i].dstBinding=bindings[i].binding;wr[i].descriptorCount=1;wr[i].descriptorType=bindings[i].descriptorType;wr[i].pBufferInfo=&infos[i];}
    vkUpdateDescriptorSets(c.device,2,wr,0,nullptr);
    VkShaderModule module=Shader(c,xbl_real_clear_spv,sizeof(xbl_real_clear_spv));
    VkComputePipelineCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;pi.layout=pl;pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=module;pi.stage.pName="main";
    VkPipeline pipeline{};c.Check(vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline),"realComputePipeline");c.cleanup.push_back([&c,pipeline]{vkDestroyPipeline(c.device,pipeline,nullptr);});
    c.Begin();
    BufferBarrier(c,support,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_UNIFORM_READ_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    BufferBarrier(c,output,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,nullptr);
    vkCmdDispatch(c.command,1,1,1);
    BufferBarrier(c,output,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT);c.Submit();Cache(c,output,true);
    c.out << "realReadback=" << Hex(output.mapped,16) << " dispatch=1,1,1 local=1,1,1\n";
    const uint32_t expected[]={0,0xffffffffu,0xffffffffu,0xffffffffu};
    c.Require(memcmp(output.mapped,expected,sizeof(expected))==0,"realClearDispatchReadback");
    c.out << "realShader=PASS gameRuntime=NOT_EXECUTED\n";
#endif
}
void Real2D(Context &c,const std::string &files,uint64_t width,uint64_t height){
#ifndef XBL_REAL_2D
    c.Require(false,"real2DExternalAssetsNotConfigured");
#else
    char error[512]{};xbg_vk_renderer *renderer=nullptr;
    xbg_vk_info info{};info.physical=c.physical;info.device=c.device;info.queue=c.queue;info.queue_family=c.family;
    info.enabled_core=c.enabledCore;info.enabled_shader_draw_parameters=c.enabledDrawParameters;
    info.vs_words=xbl_real_2d_vs;info.vs_bytes=sizeof(xbl_real_2d_vs);info.ps_words=xbl_real_2d_ps;info.ps_bytes=sizeof(xbl_real_2d_ps);info.fence_timeout_ns=5000000000ULL;
    int result=xbg_vk_create(&info,&renderer,error,sizeof(error));c.out << "createReal2D=" << result << " error=" << error << '\n';c.Require(result==0,"real2DRenderer");
    c.cleanup.push_back([renderer]{xbg_vk_destroy(renderer);});
    auto checked=[&](int code,const char *label){c.out << label << '=' << code << " error=" << xbg_vk_error(renderer) << '\n';if(code!=0){c.pending=true;throw std::runtime_error(label);}};
    xbg_vk_texture *texture=nullptr;checked(xbg_vk_upload_texture(renderer,xbl_original_logo,sizeof(xbl_original_logo),&texture),"uploadOriginalLogo");
    checked(xbg_vk_replay_texture(renderer,texture),"drawRealVSPS");
    std::vector<uint8_t> rgba(337*204*4);checked(xbg_vk_readback(renderer,rgba.data(),rgba.size()),"readbackOriginalLogo");
    const std::string path=files+"/logo-readback.rgba";FILE *f=fopen(path.c_str(),"wb");c.Require(f!=nullptr,"readbackFileOpen");size_t written=fwrite(rgba.data(),1,rgba.size(),f);int closed=fclose(f);c.Require(written==rgba.size()&&closed==0,"readbackFileWritten");
    uint32_t maxError=0,badPixels=0,alphaMin=255,alphaMax=0;
    c.Require(sizeof(xbl_logo_expected)==rgba.size(),"logoExpectedSize");
    for(size_t i=0;i<rgba.size();i+=4){bool bad=false;for(size_t ch=0;ch<4;++ch){uint32_t diff=std::abs(int(rgba[i+ch])-int(xbl_logo_expected[i+ch]));maxError=std::max(maxError,diff);bad=bad||diff>1;}badPixels+=bad;alphaMin=std::min(alphaMin,uint32_t(rgba[i+3]));alphaMax=std::max(alphaMax,uint32_t(rgba[i+3]));}
    c.out << "originalLogoReadback bytes=" << rgba.size() << " maxChannelError=" << maxError << " badPixels=" << badPixels << " alphaMin=" << alphaMin << " alphaMax=" << alphaMax << " file=" << path << '\n';
    c.Require(badPixels==0,"realShaderOriginalTextureReadback");
    uint32_t w=0,h=0;VkImage output=xbg_vk_output_image(renderer,&w,&h);c.Require(output&&w==337&&h==204,"real2DOutputImage");
    Present(c,width,height,output,w,h);
    c.out << "real2D=PASS mode=ORIGINAL_SHADER_TEXTURE_REPLAY gameAutonomousFrame=NO\n";
#endif
}
struct Work {napi_async_work handle{};napi_deferred deferred{};std::string report;bool present=false;bool real=false;bool twoD=false;std::string files;};
void ExecuteGpu(napi_env,void *data) {
    auto &w=*static_cast<Work *>(data);auto c=std::make_unique<Context>();
    try {
        if(w.twoD){
            OHNativeWindow *window=nullptr;uint64_t width=0,height=0;
            {std::lock_guard<std::mutex> lock(windowMutex);if(currentWindow&&OH_NativeWindow_NativeObjectReference(currentWindow)==0){window=currentWindow;width=windowWidth;height=windowHeight;}}
            c->Require(window!=nullptr,"nativeWindowAvailable");c->Init(window,false,true);Real2D(*c,w.files,width,height);
        }
        else if(w.real){c->Init(nullptr,true);RealClear(*c);}
        else if(w.present){
            OHNativeWindow *window=nullptr;uint64_t width=0,height=0;
            {std::lock_guard<std::mutex> lock(windowMutex);if(currentWindow && OH_NativeWindow_NativeObjectReference(currentWindow)==0){window=currentWindow;width=windowWidth;height=windowHeight;}}
            c->Require(window!=nullptr,"nativeWindowAvailable");c->Init(window);Present(*c,width,height);
        } else {c->out << "gpuProbeVersion=3 stage=AB game=NOT_INTEGRATED\n";c->Init();Compute(*c);SampleReadback(*c);c->out << "gpuProbe=PASS\n";}
    }
    catch(const std::exception &e){c->out << "gpuProbe=FAIL reason=" << e.what() << '\n';}
    catch(...){c->out << "gpuProbe=FAIL native_exception\n";}
    if(c->pending){c->out << "context=QUARANTINED restart_app_required no_reentry\n";w.report=c->out.str();quarantined=c.release();}
    else if(c->presentationOutstanding){w.report=c->out.str();presented=c.release();}
    else w.report=c->out.str();
}
void CompleteGpu(napi_env env,napi_status status,void *data) {
    std::unique_ptr<Work> w(static_cast<Work *>(data));napi_value result{};
    const std::string report=status==napi_ok?w->report:"gpuProbe=FAIL async_completion";
    if(napi_create_string_utf8(env,report.c_str(),report.size(),&result)==napi_ok) napi_resolve_deferred(env,w->deferred,result);
    napi_delete_async_work(env,w->handle);gpuBusy.store(false);
}
}
napi_value QueueGpu(napi_env env,bool present,bool real=false,const std::string &files="") {
    if(gpuBusy.exchange(true)){napi_throw_error(env,nullptr,"GPU probe is busy");return nullptr;}
    if(quarantined){gpuBusy.store(false);napi_throw_error(env,nullptr,"GPU context quarantined; restart this test app");return nullptr;}
    if((present||!files.empty()) && presented){gpuBusy.store(false);napi_throw_error(env,nullptr,"Single-frame test already submitted; restart app to repeat");return nullptr;}
    auto w=std::make_unique<Work>();napi_value promise{},name{};
    w->present=present;
    w->real=real;
    w->twoD=!files.empty();w->files=files;
    if(napi_create_string_utf8(env,"XblGpuChecks",NAPI_AUTO_LENGTH,&name)!=napi_ok ||
       napi_create_promise(env,&w->deferred,&promise)!=napi_ok ||
       napi_create_async_work(env,nullptr,name,ExecuteGpu,CompleteGpu,w.get(),&w->handle)!=napi_ok ||
       napi_queue_async_work(env,w->handle)!=napi_ok){
        if(w->handle)napi_delete_async_work(env,w->handle);gpuBusy.store(false);napi_throw_error(env,nullptr,"Failed to queue GPU probe");return nullptr;
    }
    w.release();return promise;
}
napi_value RunGpuChecks(napi_env env,napi_callback_info){return QueueGpu(env,false);}
OHNativeWindow *AcquireGameSurface(){
    if(gpuBusy.exchange(true)||quarantined||presented)return nullptr;
    std::lock_guard<std::mutex> lock(windowMutex);
    if(currentWindow&&OH_NativeWindow_NativeObjectReference(currentWindow)==0)return currentWindow;
    return nullptr;
}
bool IsGameSurfaceCurrent(OHNativeWindow *window){std::lock_guard<std::mutex> lock(windowMutex);return currentWindow==window;}
bool WaitForGameSurface(GameSurfaceSnapshot &snapshot){
    std::unique_lock<std::mutex> lock(windowMutex);
    waitingForGameSurface.store(!gameSurfaceCancelled&&!(gameSurfaceForeground&&currentWindow&&windowWidth&&windowHeight));
    windowChanged.wait(lock,[]{return gameSurfaceCancelled||(gameSurfaceForeground&&currentWindow&&windowWidth&&windowHeight);});
    waitingForGameSurface.store(false);
    if(gameSurfaceCancelled)return false;
    if(OH_NativeWindow_NativeObjectReference(currentWindow)!=0)return false;
    snapshot={currentWindow,windowWidth,windowHeight,windowGeneration};return true;
}
void SetGameSurfaceForeground(bool foreground){SetGameTouchForeground(foreground);if(!foreground)CancelSurfaceTouch();std::lock_guard<std::mutex> lock(windowMutex);gameSurfaceForeground=foreground;windowChanged.notify_all();}
bool WaitForGameSurfaceRefresh(uint64_t generation,GameSurfaceSnapshot &snapshot){
    {
        std::unique_lock<std::mutex> lock(windowMutex);
        waitingForGameSurface.store(true);
        windowChanged.wait_for(lock,std::chrono::milliseconds(250),[generation]{return gameSurfaceCancelled||!gameSurfaceForeground||windowGeneration!=generation;});
        waitingForGameSurface.store(false);
    }
    return WaitForGameSurface(snapshot);
}
void CancelGameSurfaceWait(){std::lock_guard<std::mutex> lock(windowMutex);gameSurfaceCancelled=true;windowChanged.notify_all();}
bool IsGameSurfaceWaiting(){return waitingForGameSurface.load();}
napi_value RunRealShader(napi_env env,napi_callback_info){return QueueGpu(env,false,true);}
napi_value RunReal2D(napi_env env,napi_callback_info info){
    size_t argc=1; napi_value argv[1]{};char path[1024]{};size_t n=0;
    if(napi_get_cb_info(env,info,&argc,argv,nullptr,nullptr)!=napi_ok||argc!=1||napi_get_value_string_utf8(env,argv[0],path,sizeof(path),&n)!=napi_ok||n==0||n>=sizeof(path)-1){napi_throw_error(env,nullptr,"Expected application files directory");return nullptr;}
    return QueueGpu(env,false,false,path);
}
napi_value PresentGpuTest(napi_env env,napi_callback_info){return QueueGpu(env,true);}
void BindGpuSurface(napi_env env,napi_value exports) {
    bool has=false;if(napi_has_named_property(env,exports,OH_NATIVE_XCOMPONENT_OBJ,&has)!=napi_ok||!has)return;
    napi_value object{};OH_NativeXComponent *component=nullptr;
    if(napi_get_named_property(env,exports,OH_NATIVE_XCOMPONENT_OBJ,&object)!=napi_ok ||
       napi_unwrap(env,object,reinterpret_cast<void **>(&component))!=napi_ok || !component)return;
    if(OH_NativeXComponent_RegisterCallback(component,&surfaceCallbacks)!=OH_NATIVEXCOMPONENT_RESULT_SUCCESS)
        napi_throw_error(env,nullptr,"Could not bind GPU test surface");
}

napi_value SendSurfaceTouch(napi_env env,napi_callback_info info){return SendSurfaceTouchImpl(env,info);}
