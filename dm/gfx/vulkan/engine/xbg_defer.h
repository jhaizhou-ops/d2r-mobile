// 延迟命令流（D2R60-12，DM_GFX_DEFER=1，缺省关）：翻译线程不再自己调驱动，把录命令（vkCmd*）、描述符模板更新、命令缓冲开始 / 结束与队列提交
// 记进命令流（类型擦除的小记录，参数与数组按值拷进 32 KB 的块），一条工作线程（dm-vk）按顺序执行——yuzu / Eden VulkanWorker 的做法。
// Maleoon 的驱动时间与命令量成正比（拆分提交 A/B：只是搬家），营地翻译线程里驱动占 31.8%（P4 剖析），挪到别的核上才省得下来。
// 其余要结果或改共享状态的调用（等栅栏、读查询、销毁、命令池操作、非模板描述符更新、队列等空）先 drain()：等工作线程把已记的执行完。
// 创建类调用、分配描述符集（工作线程从不碰描述符池）、内存映射不必等。上屏回调（另一个编译单元）之前也 drain。
// 辅助线程（命令池重置、管线缓存保存、着色器预热）只碰已完成的栅栏 / 命令池或新建对象，标 aux 后不 drain（它们不持引擎锁，不能碰命令流）。
// 只在 xbg_game.cpp 里生效：本头文件用同名函数式宏改写其后所有 vk 调用；(vkFoo)(...) 带括号的写法取原函数。
// Deferred command stream (D2R60-12, DM_GFX_DEFER=1, off by default): the translation thread no longer calls the driver itself. Command
// recording (vkCmd*), descriptor template updates, command buffer begin / end and queue submits go into a stream (small type-erased
// records, arguments and arrays copied by value into 32 KB chunks) that one worker thread (dm-vk) executes in order, as the yuzu / Eden
// VulkanWorker does. Maleoon driver time scales with command volume (the split-submit A/B only moved it), and the driver is 31.8% of the
// camp translation thread (P4 profile): it is only saved by running on another core. Every other call that needs a result or changes
// shared state (fence waits, query reads, destroys, command pool operations, non-template descriptor writes, queue idle) drains first:
// waits until the worker has executed everything recorded. Creates, descriptor set allocation (the worker never touches descriptor
// pools) and memory mapping need no drain. The present callback (another translation unit) is preceded by a drain as well. Auxiliary
// threads (command pool reset, pipeline cache save, shader warm-up) only touch completed fences / pools or new objects: marked aux, they
// never drain (they do not hold the engine lock and must not touch the stream).
// Only active in xbg_game.cpp: this header rewrites every later vk call there with same-named function-like macros; (vkFoo)(...) in
// parentheses reaches the real function.
#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <pthread.h>
#include <dlfcn.h>
#include <time.h>
#include <cstdlib>

extern "C" __attribute__((noreturn)) void xbg_fatalf(const char *format,...);
extern "C" void xbg_logf(const char *format,...);

namespace vkd {
inline bool on=false;                          // DM_GFX_DEFER, set once at initialization
inline thread_local bool aux=false;            // auxiliary engine threads: never drain
// Deferred submits: recorded (translation thread) / issued (worker). The command pool reset thread waits until its slot's submit is
// issued before waiting on the fence: on Maleoon a fence wait that starts before its vkQueueSubmit never wakes.
inline uint64_t submits_recorded=0;inline std::atomic<uint64_t> submits_done{0};inline std::mutex submit_mu;inline std::condition_variable submit_cv;
inline void wait_submitted(uint64_t n){if(submits_done.load()>=n)return;std::unique_lock<std::mutex> l(submit_mu);submit_cv.wait(l,[n]{return submits_done.load()>=n;});}
struct Hdr{void (*run)(Hdr*);uint32_t next;uint32_t pad;};
struct Chunk{std::vector<uint8_t> buf;size_t used=0;uint32_t first=0,last=UINT32_MAX;};
inline std::mutex mu;inline std::condition_variable cv_work,cv_idle;inline std::deque<Chunk*> todo,spare;
inline Chunk *cur=nullptr;inline std::thread *worker=nullptr;inline std::thread::id worker_id;inline bool busy=false;
inline uint64_t records=0,chunks=0,drains=0,drain_ns=0,flushes=0;inline std::atomic<uint64_t> exec_ns{0},exec_records{0},exec_cpu_ns{0};
inline uint64_t thread_cpu_ns(){timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec);}
inline uint64_t main_cpu0=0,main_wall0=0;   // translation thread CPU vs wall between reports (preemption shows as CPU << wall)
constexpr size_t CHUNK=32*1024;
inline uint64_t now_ns(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec);}
inline void worker_loop(){
    worker_id=std::this_thread::get_id();aux=true;pthread_setname_np(pthread_self(),"dm-vk");
    {const char *q=getenv("DM_GFX_DEFER_QOS");int lv=q&&*q?atoi(q):5;   // 5 USER_INTERACTIVE (big cores), 3 USER_INITIATED, 2 DEFAULT
     void *h=dlopen("libqos.so",RTLD_NOW);auto set=h?reinterpret_cast<int(*)(int)>(dlsym(h,"OH_QoS_SetThreadQoS")):nullptr;int r=set&&lv>=0?set(lv):-1;xbg_logf("NVN defer worker dm-vk QoS=%d rc=%d",lv,r);}
    for(;;){Chunk *c;
        {std::unique_lock<std::mutex> l(mu);cv_work.wait(l,[]{return !todo.empty();});c=todo.front();todo.pop_front();busy=true;}
        if(!c)return;   // stop marker
        uint64_t t0=now_ns(),c0=thread_cpu_ns(),n=0;
        for(uint32_t o=c->first;o!=UINT32_MAX;){Hdr *h=reinterpret_cast<Hdr*>(c->buf.data()+o);o=h->next;h->run(h);++n;}
        exec_ns+=now_ns()-t0;exec_cpu_ns+=thread_cpu_ns()-c0;exec_records+=n;c->used=0;c->first=0;c->last=UINT32_MAX;
        {std::lock_guard<std::mutex> l(mu);spare.push_back(c);busy=false;}cv_idle.notify_all();
    }
}
inline void start(){if(!worker){worker=new std::thread(worker_loop);}}
inline Chunk *fresh(size_t need){
    Chunk *c=nullptr;{std::lock_guard<std::mutex> l(mu);if(!spare.empty()){c=spare.back();spare.pop_back();}}
    if(!c)c=new Chunk;if(c->buf.size()<need)c->buf.resize(need<CHUNK?CHUNK:need);c->used=0;c->first=0;c->last=UINT32_MAX;return c;
}
// Hand the current chunk to the worker (no waiting).
inline void flush(){
    if(!cur || cur->last==UINT32_MAX)return;
    {std::lock_guard<std::mutex> l(mu);todo.push_back(cur);}cv_work.notify_one();cur=nullptr;++chunks;++flushes;
}
// Wait until the worker has executed everything recorded so far.
inline void drain(){
    if(!on || aux || !worker)return;
    flush();uint64_t t0=now_ns();
    {std::unique_lock<std::mutex> l(mu);if(todo.empty() && !busy)return;cv_idle.wait(l,[]{return todo.empty() && !busy;});}
    ++drains;drain_ns+=now_ns()-t0;
}
// Room for one record of `bytes` (header, closure and copied arrays) in the current chunk.
inline void reserve(size_t bytes){bytes+=64;if(!cur){cur=fresh(bytes);return;}if(cur->used+bytes>cur->buf.size()){flush();cur=fresh(bytes);}}
inline void *space(size_t bytes){size_t o=(cur->used+15)&~size_t(15);cur->used=o+bytes;return cur->buf.data()+o;}
template<class T> inline T *dup(const T *p,uint32_t n){if(!p || !n)return nullptr;T *d=static_cast<T*>(space(sizeof(T)*n));memcpy(static_cast<void*>(d),p,sizeof(T)*n);return d;}
template<class F> inline void emit(F &&f){
    using T=typename std::decay<F>::type;
    struct Rec{Hdr h;T f;};
    void *mem=space(sizeof(Rec));Rec *r=new(mem) Rec{Hdr{[](Hdr *h){Rec *x=reinterpret_cast<Rec*>(h);x->f();x->f.~T();},UINT32_MAX,0},std::forward<F>(f)};
    uint32_t off=uint32_t(reinterpret_cast<uint8_t*>(r)-cur->buf.data());
    if(cur->last==UINT32_MAX)cur->first=off;else reinterpret_cast<Hdr*>(cur->buf.data()+cur->last)->next=off;
    cur->last=off;++records;
}
inline void stop(){if(!worker)return;drain();{std::lock_guard<std::mutex> l(mu);todo.push_back(nullptr);}cv_work.notify_one();worker->join();delete worker;worker=nullptr;}
inline void check(VkResult r,const char *call){if(r!=VK_SUCCESS)xbg_fatalf("%s failed on the deferred worker: %d",call,int(r));}

// Descriptor update template data sizes (recorded at creation; the template update copies that many bytes).
inline std::mutex tmpl_mu;inline std::unordered_map<VkDescriptorUpdateTemplate,size_t> tmpl_size;
inline size_t elem_size(VkDescriptorType t){
    switch(t){case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:return sizeof(VkBufferView);
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:return sizeof(VkDescriptorBufferInfo);
    default:return sizeof(VkDescriptorImageInfo);}
}

// ---- wrappers ----
#define VKD_IMM(...) do{if(!vkd::on){__VA_ARGS__;return;}}while(0)
inline void CmdDraw(VkCommandBuffer c,uint32_t a,uint32_t b,uint32_t d,uint32_t e){VKD_IMM((vkCmdDraw)(c,a,b,d,e));reserve(64);emit([=]{(vkCmdDraw)(c,a,b,d,e);});}
inline void CmdDrawIndexed(VkCommandBuffer c,uint32_t a,uint32_t b,uint32_t d,int32_t e,uint32_t g){VKD_IMM((vkCmdDrawIndexed)(c,a,b,d,e,g));reserve(64);emit([=]{(vkCmdDrawIndexed)(c,a,b,d,e,g);});}
inline void CmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){VKD_IMM((vkCmdDispatch)(c,x,y,z));reserve(64);emit([=]{(vkCmdDispatch)(c,x,y,z);});}
inline void CmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p){VKD_IMM((vkCmdBindPipeline)(c,b,p));reserve(64);emit([=]{(vkCmdBindPipeline)(c,b,p);});}
inline void CmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t){VKD_IMM((vkCmdBindIndexBuffer)(c,b,o,t));reserve(64);emit([=]{(vkCmdBindIndexBuffer)(c,b,o,t);});}
inline void CmdBindVertexBuffers(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer *b,const VkDeviceSize *o){VKD_IMM((vkCmdBindVertexBuffers)(c,f,n,b,o));
    reserve(64+n*16);const VkBuffer *bb=dup(b,n);const VkDeviceSize *oo=dup(o,n);emit([=]{(vkCmdBindVertexBuffers)(c,f,n,bb,oo);});}
inline void CmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint bp,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *s,uint32_t nd,const uint32_t *d){
    VKD_IMM((vkCmdBindDescriptorSets)(c,bp,l,first,n,s,nd,d));
    reserve(96+n*8+nd*4);const VkDescriptorSet *ss=dup(s,n);const uint32_t *dd=dup(d,nd);emit([=]{(vkCmdBindDescriptorSets)(c,bp,l,first,n,ss,nd,dd);});}
inline void CmdSetViewport(VkCommandBuffer c,uint32_t f,uint32_t n,const VkViewport *v){VKD_IMM((vkCmdSetViewport)(c,f,n,v));reserve(64+n*sizeof(VkViewport));const VkViewport *vv=dup(v,n);emit([=]{(vkCmdSetViewport)(c,f,n,vv);});}
inline void CmdSetScissor(VkCommandBuffer c,uint32_t f,uint32_t n,const VkRect2D *v){VKD_IMM((vkCmdSetScissor)(c,f,n,v));reserve(64+n*sizeof(VkRect2D));const VkRect2D *vv=dup(v,n);emit([=]{(vkCmdSetScissor)(c,f,n,vv);});}
inline void CmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags st,uint32_t off,uint32_t size,const void *v){VKD_IMM((vkCmdPushConstants)(c,l,st,off,size,v));
    reserve(64+size);const uint8_t *vv=dup(static_cast<const uint8_t*>(v),size);emit([=]{(vkCmdPushConstants)(c,l,st,off,size,vv);});}
inline void CmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo *b,VkSubpassContents sc){VKD_IMM((vkCmdBeginRenderPass)(c,b,sc));
    if(b->pNext){drain();(vkCmdBeginRenderPass)(c,b,sc);return;}
    reserve(128+b->clearValueCount*sizeof(VkClearValue));VkRenderPassBeginInfo bi=*b;bi.pClearValues=dup(b->pClearValues,b->clearValueCount);emit([=]{(vkCmdBeginRenderPass)(c,&bi,sc);});}
inline void CmdEndRenderPass(VkCommandBuffer c){VKD_IMM((vkCmdEndRenderPass)(c));reserve(32);emit([=]{(vkCmdEndRenderPass)(c);});}
inline void CmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *i){
    VKD_IMM((vkCmdPipelineBarrier)(c,s,d,f,nm,m,nb,b,ni,i));
    reserve(128+nm*sizeof(VkMemoryBarrier)+nb*sizeof(VkBufferMemoryBarrier)+ni*sizeof(VkImageMemoryBarrier));
    const VkMemoryBarrier *mm=dup(m,nm);const VkBufferMemoryBarrier *bb=dup(b,nb);const VkImageMemoryBarrier *ii=dup(i,ni);emit([=]{(vkCmdPipelineBarrier)(c,s,d,f,nm,mm,nb,bb,ni,ii);});}
inline void CmdCopyBuffer(VkCommandBuffer c,VkBuffer s,VkBuffer d,uint32_t n,const VkBufferCopy *r){VKD_IMM((vkCmdCopyBuffer)(c,s,d,n,r));reserve(64+n*sizeof(*r));const VkBufferCopy *rr=dup(r,n);emit([=]{(vkCmdCopyBuffer)(c,s,d,n,rr);});}
inline void CmdCopyImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy *r){VKD_IMM((vkCmdCopyImage)(c,s,sl,d,dl,n,r));reserve(64+n*sizeof(*r));const VkImageCopy *rr=dup(r,n);emit([=]{(vkCmdCopyImage)(c,s,sl,d,dl,n,rr);});}
inline void CmdBlitImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageBlit *r,VkFilter f){VKD_IMM((vkCmdBlitImage)(c,s,sl,d,dl,n,r,f));reserve(64+n*sizeof(*r));const VkImageBlit *rr=dup(r,n);emit([=]{(vkCmdBlitImage)(c,s,sl,d,dl,n,rr,f);});}
inline void CmdResolveImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageResolve *r){VKD_IMM((vkCmdResolveImage)(c,s,sl,d,dl,n,r));reserve(64+n*sizeof(*r));const VkImageResolve *rr=dup(r,n);emit([=]{(vkCmdResolveImage)(c,s,sl,d,dl,n,rr);});}
inline void CmdCopyBufferToImage(VkCommandBuffer c,VkBuffer s,VkImage d,VkImageLayout l,uint32_t n,const VkBufferImageCopy *r){VKD_IMM((vkCmdCopyBufferToImage)(c,s,d,l,n,r));reserve(64+n*sizeof(*r));const VkBufferImageCopy *rr=dup(r,n);emit([=]{(vkCmdCopyBufferToImage)(c,s,d,l,n,rr);});}
inline void CmdCopyImageToBuffer(VkCommandBuffer c,VkImage s,VkImageLayout l,VkBuffer d,uint32_t n,const VkBufferImageCopy *r){VKD_IMM((vkCmdCopyImageToBuffer)(c,s,l,d,n,r));reserve(64+n*sizeof(*r));const VkBufferImageCopy *rr=dup(r,n);emit([=]{(vkCmdCopyImageToBuffer)(c,s,l,d,n,rr);});}
inline void CmdClearAttachments(VkCommandBuffer c,uint32_t na,const VkClearAttachment *a,uint32_t nr,const VkClearRect *r){VKD_IMM((vkCmdClearAttachments)(c,na,a,nr,r));
    reserve(64+na*sizeof(*a)+nr*sizeof(*r));const VkClearAttachment *aa=dup(a,na);const VkClearRect *rr=dup(r,nr);emit([=]{(vkCmdClearAttachments)(c,na,aa,nr,rr);});}
inline void CmdClearColorImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue *v,uint32_t n,const VkImageSubresourceRange *r){VKD_IMM((vkCmdClearColorImage)(c,i,l,v,n,r));
    reserve(96+n*sizeof(*r));VkClearColorValue vv=*v;const VkImageSubresourceRange *rr=dup(r,n);emit([=]{(vkCmdClearColorImage)(c,i,l,&vv,n,rr);});}
inline void CmdClearDepthStencilImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearDepthStencilValue *v,uint32_t n,const VkImageSubresourceRange *r){VKD_IMM((vkCmdClearDepthStencilImage)(c,i,l,v,n,r));
    reserve(96+n*sizeof(*r));VkClearDepthStencilValue vv=*v;const VkImageSubresourceRange *rr=dup(r,n);emit([=]{(vkCmdClearDepthStencilImage)(c,i,l,&vv,n,rr);});}
inline void CmdBeginQuery(VkCommandBuffer c,VkQueryPool p,uint32_t q,VkQueryControlFlags f){VKD_IMM((vkCmdBeginQuery)(c,p,q,f));reserve(48);emit([=]{(vkCmdBeginQuery)(c,p,q,f);});}
inline void CmdEndQuery(VkCommandBuffer c,VkQueryPool p,uint32_t q){VKD_IMM((vkCmdEndQuery)(c,p,q));reserve(48);emit([=]{(vkCmdEndQuery)(c,p,q);});}
inline void CmdResetQueryPool(VkCommandBuffer c,VkQueryPool p,uint32_t f,uint32_t n){VKD_IMM((vkCmdResetQueryPool)(c,p,f,n));reserve(48);emit([=]{(vkCmdResetQueryPool)(c,p,f,n);});}
inline void CmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits s,VkQueryPool p,uint32_t q){VKD_IMM((vkCmdWriteTimestamp)(c,s,p,q));reserve(48);emit([=]{(vkCmdWriteTimestamp)(c,s,p,q);});}
inline void CmdCopyQueryPoolResults(VkCommandBuffer c,VkQueryPool p,uint32_t f,uint32_t n,VkBuffer b,VkDeviceSize o,VkDeviceSize st,VkQueryResultFlags fl){VKD_IMM((vkCmdCopyQueryPoolResults)(c,p,f,n,b,o,st,fl));
    reserve(80);emit([=]{(vkCmdCopyQueryPoolResults)(c,p,f,n,b,o,st,fl);});}
inline VkResult BeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *b){if(!on || b->pNext || b->pInheritanceInfo){drain();return (vkBeginCommandBuffer)(c,b);}
    reserve(96);VkCommandBufferBeginInfo bi=*b;emit([=]{check((vkBeginCommandBuffer)(c,&bi),"vkBeginCommandBuffer");});return VK_SUCCESS;}
inline VkResult EndCommandBuffer(VkCommandBuffer c){if(!on)return (vkEndCommandBuffer)(c);reserve(32);emit([=]{check((vkEndCommandBuffer)(c),"vkEndCommandBuffer");});return VK_SUCCESS;}
inline VkResult ResetCommandBuffer(VkCommandBuffer c,VkCommandBufferResetFlags f){if(!on)return (vkResetCommandBuffer)(c,f);reserve(32);emit([=]{check((vkResetCommandBuffer)(c,f),"vkResetCommandBuffer");});return VK_SUCCESS;}
inline void UpdateDescriptorSetWithTemplate(VkDevice dev,VkDescriptorSet s,VkDescriptorUpdateTemplate t,const void *data){
    if(!on){(vkUpdateDescriptorSetWithTemplate)(dev,s,t,data);return;}
    size_t n=0;{std::lock_guard<std::mutex> l(tmpl_mu);auto it=tmpl_size.find(t);if(it!=tmpl_size.end())n=it->second;}
    if(!n){drain();(vkUpdateDescriptorSetWithTemplate)(dev,s,t,data);return;}
    reserve(64+n);const uint8_t *dd=dup(static_cast<const uint8_t*>(data),uint32_t(n));emit([=]{(vkUpdateDescriptorSetWithTemplate)(dev,s,t,dd);});}
inline VkResult CreateDescriptorUpdateTemplate(VkDevice d,const VkDescriptorUpdateTemplateCreateInfo *ci,const VkAllocationCallbacks *a,VkDescriptorUpdateTemplate *out){
    VkResult r=(vkCreateDescriptorUpdateTemplate)(d,ci,a,out);
    if(r==VK_SUCCESS){size_t n=0;for(uint32_t i=0;i<ci->descriptorUpdateEntryCount;i++){const auto &e=ci->pDescriptorUpdateEntries[i];if(!e.descriptorCount)continue;
            size_t end=e.descriptorType==VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK?e.offset+e.descriptorCount:e.offset+e.stride*(e.descriptorCount-1)+elem_size(e.descriptorType);if(end>n)n=end;}
        std::lock_guard<std::mutex> l(tmpl_mu);tmpl_size[*out]=n;}
    return r;}
inline void DestroyDescriptorUpdateTemplate(VkDevice d,VkDescriptorUpdateTemplate t,const VkAllocationCallbacks *a){drain();{std::lock_guard<std::mutex> l(tmpl_mu);tmpl_size.erase(t);}(vkDestroyDescriptorUpdateTemplate)(d,t,a);}
// Queue submit: arrays copied; the worker submits in stream order and counts it (submits_done).
inline VkResult QueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *si,VkFence f){
    if(!on)return (vkQueueSubmit)(q,n,si,f);
    for(uint32_t i=0;i<n;i++)if(si[i].pNext){drain();return (vkQueueSubmit)(q,n,si,f);}
    size_t bytes=128+n*sizeof(VkSubmitInfo);for(uint32_t i=0;i<n;i++)bytes+=si[i].waitSemaphoreCount*(sizeof(VkSemaphore)+sizeof(VkPipelineStageFlags))+si[i].commandBufferCount*sizeof(VkCommandBuffer)+si[i].signalSemaphoreCount*sizeof(VkSemaphore)+64;
    reserve(bytes);VkSubmitInfo *ss=dup(si,n);
    for(uint32_t i=0;i<n;i++){ss[i].pWaitSemaphores=dup(si[i].pWaitSemaphores,si[i].waitSemaphoreCount);ss[i].pWaitDstStageMask=dup(si[i].pWaitDstStageMask,si[i].waitSemaphoreCount);
        ss[i].pCommandBuffers=dup(si[i].pCommandBuffers,si[i].commandBufferCount);ss[i].pSignalSemaphores=dup(si[i].pSignalSemaphores,si[i].signalSemaphoreCount);}
    ++submits_recorded;emit([=]{check((vkQueueSubmit)(q,n,ss,f),"vkQueueSubmit");{std::lock_guard<std::mutex> l(submit_mu);++submits_done;}submit_cv.notify_all();});
    flush();   // start the GPU work promptly
    return VK_SUCCESS;}
// vkGetFenceStatus never waits: hand the stream over so a polled submit eventually happens, then ask.
inline VkResult GetFenceStatus(VkDevice d,VkFence f){if(on && !aux)flush();return (vkGetFenceStatus)(d,f);}
#undef VKD_IMM
// Direct (not deferred, not drained) driver calls on the translation thread while deferring: count and time per function (contention probe).
struct Direct{const char *name;uint64_t n,ns;};inline Direct direct[24];inline int direct_n=0;
inline Direct &direct_slot(const char *name){for(int i=0;i<direct_n;i++)if(direct[i].name==name)return direct[i];if(direct_n<24){direct[direct_n]={name,0,0};return direct[direct_n++];}return direct[23];}
template<class F> inline auto timed(const char *name,F f)->decltype(f()){if(!on || aux)return f();uint64_t t0=now_ns();struct G{const char *n;uint64_t t0;~G(){Direct &d=direct_slot(n);++d.n;d.ns+=now_ns()-t0;}} g{name,t0};return f();}
inline void report(double frames){
    if(!on || frames<=0)return;
    uint64_t mc=thread_cpu_ns(),mw=now_ns();double main_cpu=main_cpu0?(mc-main_cpu0)/1e6/frames:0,main_wall=main_wall0?(mw-main_wall0)/1e6/frames:0;main_cpu0=mc;main_wall0=mw;
    xbg_logf("NVN PERF defer records=%.0f chunks=%.1f drains=%.1f drain_ms=%.2f worker_ms=%.2f worker_cpu_ms=%.2f main_cpu_ms=%.2f main_wall_ms=%.2f",records/frames,chunks/frames,drains/frames,drain_ns/1e6/frames,exec_ns.exchange(0)/1e6/frames,exec_cpu_ns.exchange(0)/1e6/frames,main_cpu,main_wall);
    {char line[900];int o=snprintf(line,sizeof line,"NVN PERF defer_direct");for(int i=0;i<direct_n && o<(int)sizeof line-60;i++){o+=snprintf(line+o,sizeof line-o," %s=%.1f/%.2fms",direct[i].name,direct[i].n/frames,direct[i].ns/1e6/frames);direct[i].n=direct[i].ns=0;}xbg_logf("%s",line);}
    records=chunks=drains=drain_ns=flushes=0;exec_records=0;
}
}   // namespace vkd

#define vkCmdDraw(...) vkd::CmdDraw(__VA_ARGS__)
#define vkCmdDrawIndexed(...) vkd::CmdDrawIndexed(__VA_ARGS__)
#define vkCmdDispatch(...) vkd::CmdDispatch(__VA_ARGS__)
#define vkCmdBindPipeline(...) vkd::CmdBindPipeline(__VA_ARGS__)
#define vkCmdBindIndexBuffer(...) vkd::CmdBindIndexBuffer(__VA_ARGS__)
#define vkCmdBindVertexBuffers(...) vkd::CmdBindVertexBuffers(__VA_ARGS__)
#define vkCmdBindDescriptorSets(...) vkd::CmdBindDescriptorSets(__VA_ARGS__)
#define vkCmdSetViewport(...) vkd::CmdSetViewport(__VA_ARGS__)
#define vkCmdSetScissor(...) vkd::CmdSetScissor(__VA_ARGS__)
#define vkCmdPushConstants(...) vkd::CmdPushConstants(__VA_ARGS__)
#define vkCmdBeginRenderPass(...) vkd::CmdBeginRenderPass(__VA_ARGS__)
#define vkCmdEndRenderPass(...) vkd::CmdEndRenderPass(__VA_ARGS__)
#define vkCmdPipelineBarrier(...) vkd::CmdPipelineBarrier(__VA_ARGS__)
#define vkCmdCopyBuffer(...) vkd::CmdCopyBuffer(__VA_ARGS__)
#define vkCmdCopyImage(...) vkd::CmdCopyImage(__VA_ARGS__)
#define vkCmdBlitImage(...) vkd::CmdBlitImage(__VA_ARGS__)
#define vkCmdResolveImage(...) vkd::CmdResolveImage(__VA_ARGS__)
#define vkCmdCopyBufferToImage(...) vkd::CmdCopyBufferToImage(__VA_ARGS__)
#define vkCmdCopyImageToBuffer(...) vkd::CmdCopyImageToBuffer(__VA_ARGS__)
#define vkCmdClearAttachments(...) vkd::CmdClearAttachments(__VA_ARGS__)
#define vkCmdClearColorImage(...) vkd::CmdClearColorImage(__VA_ARGS__)
#define vkCmdClearDepthStencilImage(...) vkd::CmdClearDepthStencilImage(__VA_ARGS__)
#define vkCmdBeginQuery(...) vkd::CmdBeginQuery(__VA_ARGS__)
#define vkCmdEndQuery(...) vkd::CmdEndQuery(__VA_ARGS__)
#define vkCmdResetQueryPool(...) vkd::CmdResetQueryPool(__VA_ARGS__)
#define vkCmdWriteTimestamp(...) vkd::CmdWriteTimestamp(__VA_ARGS__)
#define vkCmdCopyQueryPoolResults(...) vkd::CmdCopyQueryPoolResults(__VA_ARGS__)
#define vkBeginCommandBuffer(...) vkd::BeginCommandBuffer(__VA_ARGS__)
#define vkEndCommandBuffer(...) vkd::EndCommandBuffer(__VA_ARGS__)
#define vkResetCommandBuffer(...) vkd::ResetCommandBuffer(__VA_ARGS__)
#define vkUpdateDescriptorSetWithTemplate(...) vkd::UpdateDescriptorSetWithTemplate(__VA_ARGS__)
#define vkCreateDescriptorUpdateTemplate(...) vkd::CreateDescriptorUpdateTemplate(__VA_ARGS__)
#define vkDestroyDescriptorUpdateTemplate(...) vkd::DestroyDescriptorUpdateTemplate(__VA_ARGS__)
#define vkQueueSubmit(...) vkd::QueueSubmit(__VA_ARGS__)
#define vkGetFenceStatus(...) vkd::GetFenceStatus(__VA_ARGS__)
// Direct calls, timed while deferring (contention probe).
#define VKD_TIMED(fn, ...) vkd::timed(#fn,[&]{return (fn)(__VA_ARGS__);})
#define vkAllocateDescriptorSets(...) VKD_TIMED(vkAllocateDescriptorSets, __VA_ARGS__)
#define vkMapMemory(...) VKD_TIMED(vkMapMemory, __VA_ARGS__)
#define vkUnmapMemory(...) VKD_TIMED(vkUnmapMemory, __VA_ARGS__)
#define vkFlushMappedMemoryRanges(...) VKD_TIMED(vkFlushMappedMemoryRanges, __VA_ARGS__)
#define vkInvalidateMappedMemoryRanges(...) VKD_TIMED(vkInvalidateMappedMemoryRanges, __VA_ARGS__)
#define vkCreateBuffer(...) VKD_TIMED(vkCreateBuffer, __VA_ARGS__)
#define vkCreateBufferView(...) VKD_TIMED(vkCreateBufferView, __VA_ARGS__)
#define vkCreateImage(...) VKD_TIMED(vkCreateImage, __VA_ARGS__)
#define vkCreateImageView(...) VKD_TIMED(vkCreateImageView, __VA_ARGS__)
#define vkCreateFramebuffer(...) VKD_TIMED(vkCreateFramebuffer, __VA_ARGS__)
#define vkCreateGraphicsPipelines(...) VKD_TIMED(vkCreateGraphicsPipelines, __VA_ARGS__)
#define vkCreateSampler(...) VKD_TIMED(vkCreateSampler, __VA_ARGS__)
#define vkAllocateMemory(...) VKD_TIMED(vkAllocateMemory, __VA_ARGS__)
#define vkBindBufferMemory(...) VKD_TIMED(vkBindBufferMemory, __VA_ARGS__)
#define vkBindImageMemory(...) VKD_TIMED(vkBindImageMemory, __VA_ARGS__)
#define vkGetBufferMemoryRequirements(...) VKD_TIMED(vkGetBufferMemoryRequirements, __VA_ARGS__)
// Drain first: results, shared state, destruction.
#define VKD_DRAIN(fn, ...) (vkd::drain(), (fn)(__VA_ARGS__))
#define vkWaitForFences(...) VKD_DRAIN(vkWaitForFences, __VA_ARGS__)
#define vkResetFences(...) VKD_DRAIN(vkResetFences, __VA_ARGS__)
#define vkQueueWaitIdle(...) VKD_DRAIN(vkQueueWaitIdle, __VA_ARGS__)
#define vkGetQueryPoolResults(...) VKD_DRAIN(vkGetQueryPoolResults, __VA_ARGS__)
#define vkResetCommandPool(...) VKD_DRAIN(vkResetCommandPool, __VA_ARGS__)
#define vkAllocateCommandBuffers(...) VKD_DRAIN(vkAllocateCommandBuffers, __VA_ARGS__)
#define vkUpdateDescriptorSets(...) VKD_DRAIN(vkUpdateDescriptorSets, __VA_ARGS__)
#define vkFreeMemory(...) VKD_DRAIN(vkFreeMemory, __VA_ARGS__)
#define vkDestroyBuffer(...) VKD_DRAIN(vkDestroyBuffer, __VA_ARGS__)
#define vkDestroyBufferView(...) VKD_DRAIN(vkDestroyBufferView, __VA_ARGS__)
#define vkDestroyCommandPool(...) VKD_DRAIN(vkDestroyCommandPool, __VA_ARGS__)
#define vkDestroyDescriptorPool(...) VKD_DRAIN(vkDestroyDescriptorPool, __VA_ARGS__)
#define vkDestroyDescriptorSetLayout(...) VKD_DRAIN(vkDestroyDescriptorSetLayout, __VA_ARGS__)
#define vkDestroyFence(...) VKD_DRAIN(vkDestroyFence, __VA_ARGS__)
#define vkDestroyFramebuffer(...) VKD_DRAIN(vkDestroyFramebuffer, __VA_ARGS__)
#define vkDestroyImage(...) VKD_DRAIN(vkDestroyImage, __VA_ARGS__)
#define vkDestroyImageView(...) VKD_DRAIN(vkDestroyImageView, __VA_ARGS__)
#define vkDestroyPipeline(...) VKD_DRAIN(vkDestroyPipeline, __VA_ARGS__)
#define vkDestroyPipelineCache(...) VKD_DRAIN(vkDestroyPipelineCache, __VA_ARGS__)
#define vkDestroyPipelineLayout(...) VKD_DRAIN(vkDestroyPipelineLayout, __VA_ARGS__)
#define vkDestroyQueryPool(...) VKD_DRAIN(vkDestroyQueryPool, __VA_ARGS__)
#define vkDestroyRenderPass(...) VKD_DRAIN(vkDestroyRenderPass, __VA_ARGS__)
#define vkDestroySampler(...) VKD_DRAIN(vkDestroySampler, __VA_ARGS__)
#define vkDestroyShaderModule(...) VKD_DRAIN(vkDestroyShaderModule, __VA_ARGS__)
