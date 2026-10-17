#define VK_USE_PLATFORM_OHOS 1

#include "game_graphics.h"
// Matching graphics API contract, provided by the paired candidate.
extern "C" int xbg_game_profile_draw_live(uint32_t frame_count);

#include "runtime_bridge.h"

#include "presentation_policy.h"

#include "gpu_checks.h"

#include "gfx_provenance_once.h"

#include <unistd.h>

#include <vulkan/vulkan.h>

#include <native_window/external_window.h>

#include <array>

#include <vector>

#include <stdexcept>

#include <cstdio>

#include <mutex>

#include <algorithm>

#include <atomic>

#include <cstring>

#include <chrono>

#include <thread>

#include <cerrno>

namespace {std::atomic<uint64_t> presentedFrames{0};}
std::atomic<float> gContentX0{0.0f},gContentW{1.0f};
// Surface-normalised x -> game-image-normalised x (pillarbox on wide screens). False when outside the image.
bool XblContentX(float &nx){const float x0=gContentX0.load(),cw=gContentW.load();if(cw<=0)return false;const float v=(nx-x0)/cw;if(v<0||v>1)return false;nx=v;return true;}
void XblContentRect(float &x0,float &cw){x0=gContentX0.load();cw=gContentW.load();}

#ifdef XBL_GAME_GRAPHICS

#include "xbg_game.h"

namespace {

// A fatal guest unwind can retain locks and in-flight resources. This one-shot

// session intentionally lives until process exit, including on any failure.

#include <stdint.h>

// Present-callback thread only, serialized by the existing backend lock.
// No timer, allocation, GPU query, readback, IO, or input side effects here.
struct XblPresentTimingOnce {
    enum Part { AcquireCall, AcquireFence, BlitFence, Pacing, QueuePresent, PartCount };
    struct Counter { uint64_t calls=0,total_ns=0,max_ns=0; } part[PartCount];
    bool attempted=false,active=false,has_end=false,stable=true;
    uint64_t began=0,callback_start=0,last_end=0,start_frames=0,end_frames=0,start_generation=0;
    uint64_t callbacks=0,callback_ns=0,callback_max_ns=0,outside_ns=0,outside_max_ns=0;
    int final_rc=0,reason=0; // 1 time bound, 2 callback bound, 3 error
    bool arm(uint64_t now,uint64_t frames,uint64_t generation){
        if(attempted)return false;
        attempted=active=true;began=now;start_frames=end_frames=frames;start_generation=generation;
        return true;
    }
    void enter(uint64_t now){
        if(!active)return;
        if(has_end){
            const uint64_t gap=now>=last_end?now-last_end:0;
            outside_ns+=gap;if(gap>outside_max_ns)outside_max_ns=gap;
            if(now<last_end)stable=false;
        }
        callback_start=now;
    }
    void add(Part which,uint64_t start,uint64_t end){
        if(!active||which<0||which>=PartCount)return;
        if(end<start){stable=false;return;}
        const uint64_t elapsed=end-start;auto &c=part[which];++c.calls;c.total_ns+=elapsed;
        if(elapsed>c.max_ns)c.max_ns=elapsed;
    }
    bool leave(uint64_t now,int rc,uint64_t frames,uint64_t generation){
        if(!active)return false;
        if(now<callback_start||now<began||frames<start_frames)stable=false;
        const uint64_t elapsed=now>=callback_start?now-callback_start:0;
        ++callbacks;callback_ns+=elapsed;if(elapsed>callback_max_ns)callback_max_ns=elapsed;
        end_frames=frames;last_end=now;has_end=true;final_rc=rc;
        if(generation!=start_generation)stable=false;
        reason=rc?3:(callbacks>=64?2:(now>=began&&now-began>=20000000000ULL?1:0));
        if(!reason)return false;
        active=false;return true;
    }
};

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Callback-thread-owned receipt only; it never calls graphics or changes input.
struct XblLiveCpuReceipt {
    bool attempted=false,owned=false,invalid=false,truncated=false,armed=false,header=false,cache=false;
    unsigned lines=0,bytes=0,parts=0,requested=0;
    uint64_t first=0,last=0,frames=0;
    char reason[32]{};
    bool begin(unsigned count){
        if(attempted||count<1||count>60)return false;
        attempted=owned=true;requested=count;return true;
    }
    void refused(){owned=false;}
    bool complete()const{return !invalid&&!truncated&&armed&&header&&cache&&parts==0x3fff&&lines==17;}
    bool accept(const char *message){
        static const char prefix[]="NVN CPU_TIMING ";
        if(!owned||!message||strncmp(message,prefix,sizeof(prefix)-1))return false;
        const size_t n=strlen(message);
        if(lines>=20||n+1>8192-bytes){truncated=true;return false;}
        if(strchr(message,'\n')||strchr(message,'\r')||n>1023){invalid=true;return false;}
        const char *s=message+sizeof(prefix)-1;int used=0;
        unsigned count=0;unsigned long long a=0,b=0,c=0,d=0,e=0,f=0,g=0,h=0,i=0,j=0;
        char name[32]{};
        bool known=false;
        if(sscanf(s,"armed-live first=%llu count=%u maxSeconds=60 clock=steady elapsed-not-thread-cpu once=1%n",&a,&count,&used)==2&&used>0&&!s[used]){
            known=true;if(armed||a==0||count!=requested)invalid=true;
            armed=true;first=a;
        }else if((used=0,sscanf(s,"v=1 reason=%31s first=%llu last=%llu frames=%llu wall_ns=%llu measured_ns=%llu uncovered_ns=%llu draws=%llu dispatches=%llu submissions=%llu shaders=%llu%n",name,&a,&b,&c,&d,&e,&f,&g,&h,&i,&j,&used))==11&&used>0&&!s[used]){
            known=true;
            if(header||!armed||a!=first||!c||c>requested||b<a||b-a+1!=c||e>d||f!=d-e)invalid=true;
            if(strcmp(name,"frames")&&strcmp(name,"60s-next-present")&&strcmp(name,"shutdown-partial"))invalid=true;
            if(!strcmp(name,"frames")&&c!=requested)invalid=true;
            header=true;last=b;frames=c;strcpy(reason,name);
        }else if((used=0,sscanf(s,"pipeline_cache hits=%llu misses=%llu evictions=%llu bypass=%llu entries=%llu limit=%llu%n",&a,&b,&c,&d,&e,&f,&used))==6&&used>0&&!s[used]){
            known=true;if(!header||cache)invalid=true;cache=true;
        }else if((used=0,sscanf(s,"part=%31s calls=%llu exclusive_ns=%llu inclusive_ns=%llu max_ns=%llu%n",name,&a,&b,&c,&d,&used))==5&&used>0&&!s[used]){
            static const char *names[]={"other","draw","dispatch","descriptor","pass","buffer","pipeline","submit","fence","present","cleanup","hash","image","log"};
            for(unsigned k=0;k<14;k++)if(!strcmp(name,names[k])){
                known=true;if(!header||!cache||(parts&(1u<<k))||b>c)invalid=true;parts|=1u<<k;break;
            }
        }
        if(!known){invalid=true;return false;}
        ++lines;bytes+=unsigned(n+1);return true;
    }
};


// Callback-thread owned. New detail mode never changes the legacy CPU receipt.
struct XblDescriptorReceiptV2 {
    XblLiveCpuReceipt cpu;
    bool invalid=false,truncated=false,armed=false,header=false;
    unsigned lines=0,bytes=0,parts=0;
    uint64_t detailFirst=0,detailLast=0,descriptor=0,tagged=0,remaining=0;
    uint64_t mainSum=0,mainDescriptor=0,mainMeasured=0,mainWall=0,mainUncovered=0;
    uint64_t taggedSum=0,remainingPart=0,clockReads=0,taggedCalls=0;
    bool begin(unsigned count){return cpu.begin(count);}
    void refused(){cpu.refused();}
    static bool add(uint64_t &a,uint64_t b){if(b>UINT64_MAX-a)return false;a+=b;return true;}
    static bool number(const char *s,uint64_t &out){
        if(!s||!*s)return false;out=0;
        for(;*s&&*s!=' ';++s){if(*s<'0'||*s>'9')return false;
            const unsigned d=unsigned(*s-'0');if(out>(UINT64_MAX-d)/10)return false;out=out*10+d;}
        return true;
    }
    static bool value(const char *row,const char *key,uint64_t &out){
        const size_t n=strlen(key);
        for(const char *p=row;*p;){if(!strncmp(p,key,n)&&p[n]=='=')return number(p+n+1,out);
            const char *space=strchr(p,' ');if(!space)break;p=space+1;}
        return false;
    }
    static bool numericFields(const char *s){
        for(const char *p=s;*p;){const char *end=strchr(p,' ');const char *eq=strchr(p,'=');
            if(eq&&(!end||eq<end)){
                // Only these schema fields are textual; all other values are strict u64.
                const size_t k=size_t(eq-p);
                const bool word=(k==6&&!strncmp(p,"reason",6))||(k==4&&!strncmp(p,"part",4))||
                    (k==5&&!strncmp(p,"clock",5))||(k==8&&!strncmp(p,"coverage",8));
                uint64_t v=0;if(!word&&!number(eq+1,v))return false;
            }
            if(!end)break;p=end+1;
        }return true;
    }
    bool complete()const{
        return !invalid&&!truncated&&cpu.complete()&&armed&&header&&parts==255&&lines==27&&
            detailFirst==cpu.first&&detailLast==cpu.last&&descriptor==mainDescriptor&&
            mainSum==mainMeasured&&mainMeasured<=mainWall&&mainUncovered==mainWall-mainMeasured&&
            taggedCalls<=UINT64_MAX/2&&clockReads==taggedCalls*2&&taggedSum==tagged&&remainingPart==remaining&&tagged<=descriptor&&remaining==descriptor-tagged;
    }
    bool accept(const char *message){
        if(!cpu.owned||!message)return false;
        const bool main=!strncmp(message,"NVN CPU_TIMING ",15);
        const bool detail=!strncmp(message,"NVN DESC_TIMING ",16);
        if(!main&&!detail)return false;
        const size_t n=strlen(message);
        if(lines>=32||n+1>16384-bytes){truncated=true;return false;}
        ++lines;bytes+=unsigned(n+1);
        if(n>1023||strchr(message,'\n')||strchr(message,'\r')||!numericFields(message+(main?15:16))){invalid=true;return false;}
        if(main){
            if(!cpu.accept(message)){invalid=true;return false;}
            const char *s=message+15;
            if(!strncmp(s,"armed-live ",11)){
                uint64_t count=0;if(!value(s,"count",count)||count!=cpu.requested)invalid=true;
            }else if(!strncmp(s,"v=1 ",4)){
                if(!value(s,"wall_ns",mainWall)||!value(s,"measured_ns",mainMeasured)||!value(s,"uncovered_ns",mainUncovered))invalid=true;
            }else if(!strncmp(s,"part=",5)){
                uint64_t v=0;if(!value(s,"exclusive_ns",v)||!add(mainSum,v))invalid=true;
                if(!strncmp(s,"part=descriptor ",16))mainDescriptor=v;
            }
            if(cpu.invalid||cpu.truncated)invalid=true;
        }else{
            const char *s=message+16;int used=0;unsigned long long a=0,b=0,c=0,d=0,e=0;
            char name[32]{},coverage[16]{};
            if(sscanf(s,"armed v=2 first=%llu count=%llu once=1 maxSeconds=60%n",&a,&b,&used)==2&&used>0&&!s[used]){
                if(armed||!a||b!=cpu.requested)invalid=true;armed=true;detailFirst=a;
            }else if((used=0,sscanf(s,"v=2 first=%llu last=%llu descriptor_ns=%llu tagged_ns=%llu remaining_ns=%llu coverage=%15s clock_reads=%31s%n",&a,&b,&c,&d,&e,coverage,name,&used))==7&&used>0&&!s[used]){
                if(header||!armed||a!=detailFirst||b<a||strcmp(coverage,"OK")||!number(name,clockReads)||d>c||e!=c-d)invalid=true;
                header=true;detailLast=b;descriptor=c;tagged=d;remaining=e;
            }else if((used=0,sscanf(s,"part=%31s calls=%llu exclusive_ns=%llu%n",name,&a,&b,&used))==3&&used>0&&!s[used]){
                static const char *names[]={"setup","buffer_bind","image_lookup","image_transition","image_view","sampler_bind","element_rest","remaining"};
                unsigned k=0;for(;k<8;++k)if(!strcmp(name,names[k]))break;
                if(k==8||!header||(k<8&&(parts&(1u<<k))))invalid=true;
                else{parts|=1u<<k;if(k==7){if(a)invalid=true;remainingPart=b;}else if(!add(taggedSum,b)||!add(taggedCalls,a))invalid=true;}
            }else invalid=true;
        }
        if(header&&cpu.header&&(detailFirst!=cpu.first||detailLast!=cpu.last))invalid=true;
        if(lines==27&&!complete())invalid=true;
        return !invalid;
    }
};

inline bool XblGraphicsFatal(const char *message){return message&&strstr(message,"FATAL");}


// Callback-thread-owned; separate schema, no backend calls and no rearm.
struct XblDrawReceiptV1 {
    XblLiveCpuReceipt cpu;
    bool invalid=false,truncated=false,armed=false,header=false,counters=false;
    unsigned lines=0,bytes=0,parts=0;
    uint64_t detailFirst=0,detailLast=0,draw=0,tagged=0,remaining=0,clockReads=0,flags=0;
    uint64_t mainSum=0,mainDraw=0,mainWall=0,mainMeasured=0,mainUncovered=0;
    uint64_t taggedSum=0,taggedCalls=0,remainingPart=0;
    uint64_t storageRanges=0,imagesVisited=0,writebackBytes=0,indexExpandCalls=0,indexExpandBytes=0;
    bool begin(unsigned count){return cpu.begin(count);}
    void refused(){cpu.refused();}
    static bool add(uint64_t &a,uint64_t b){if(b>UINT64_MAX-a)return false;a+=b;return true;}
    static bool number(const char *s,uint64_t &out){
        if(!s||!*s||(s[0]=='0'&&s[1]&&s[1]!=' '))return false;out=0;
        for(;*s&&*s!=' ';++s){if(*s<'0'||*s>'9')return false;
            const unsigned d=unsigned(*s-'0');if(out>(UINT64_MAX-d)/10)return false;out=out*10+d;}
        return true;
    }
    static bool value(const char *row,const char *key,uint64_t &out){
        const size_t n=strlen(key);
        for(const char *p=row;*p;){if(!strncmp(p,key,n)&&p[n]=='=')return number(p+n+1,out);
            const char *space=strchr(p,' ');if(!space)break;p=space+1;}
        return false;
    }
    static bool numericFields(const char *s){
        for(const char *p=s;*p;++p){if((*p>0&&*p<32)||(*p==' '&&(!p[1]||p[1]==' ')))return false;}
        for(const char *p=s;*p;){const char *end=strchr(p,' ');const char *eq=strchr(p,'=');
            if(eq&&(!end||eq<end)){
                const size_t k=size_t(eq-p);
                const bool word=(k==6&&!strncmp(p,"reason",6))||(k==4&&!strncmp(p,"part",4))||
                    (k==5&&!strncmp(p,"clock",5))||(k==8&&!strncmp(p,"coverage",8));
                uint64_t v=0;if(!word&&!number(eq+1,v))return false;
            }
            if(!end)break;p=end+1;
        }return true;
    }
    bool complete()const{
        return !invalid&&!truncated&&cpu.complete()&&armed&&header&&counters&&parts==7&&lines==23&&flags==0&&
            indexExpandBytes%2==0&&((indexExpandCalls==0)==(indexExpandBytes==0))&&((storageRanges==0)==(writebackBytes==0))&&
            detailFirst==cpu.first&&detailLast==cpu.last&&draw==mainDraw&&
            mainSum==mainMeasured&&mainMeasured<=mainWall&&mainUncovered==mainWall-mainMeasured&&
            taggedCalls<=UINT64_MAX/2&&clockReads==taggedCalls*2&&taggedSum==tagged&&
            remainingPart==remaining&&tagged<=draw&&remaining==draw-tagged;
    }
    bool accept(const char *message){
        if(!cpu.owned||!message)return false;
        const bool main=!strncmp(message,"NVN CPU_TIMING ",15),detail=!strncmp(message,"NVN DRAW_TIMING ",16);
        if(!main&&!detail)return false;
        const size_t n=strlen(message);
        if(lines>=32||bytes>16384||n+1>16384-bytes){truncated=true;return false;}
        ++lines;bytes+=unsigned(n+1);
        if(n>1023||strchr(message,'\n')||strchr(message,'\r')||!numericFields(message+(main?15:16))){invalid=true;return false;}
        if(main){
            if(!cpu.accept(message)){invalid=true;return false;}
            const char *s=message+15;
            if(!strncmp(s,"armed-live ",11)){
                uint64_t count=0;if(!value(s,"count",count)||count!=cpu.requested)invalid=true;
            }else if(!strncmp(s,"v=1 ",4)){
                if(!value(s,"wall_ns",mainWall)||!value(s,"measured_ns",mainMeasured)||!value(s,"uncovered_ns",mainUncovered))invalid=true;
            }else if(!strncmp(s,"part=",5)){
                uint64_t v=0;if(!value(s,"exclusive_ns",v)||!add(mainSum,v))invalid=true;
                if(!strncmp(s,"part=draw ",10))mainDraw=v;
            }
            if(cpu.invalid||cpu.truncated)invalid=true;
        }else{
            const char *s=message+16;int used=0;
            unsigned long long a=0,b=0,c=0,d=0,e=0,f=0,g=0;char name[32]{},coverage[16]{};
            if(sscanf(s,"armed v=1 first=%llu count=%llu once=1 maxSeconds=60%n",&a,&b,&used)==2&&used>0&&!s[used]){
                if(armed||!a||b!=cpu.requested)invalid=true;armed=true;detailFirst=a;
            }else if((used=0,sscanf(s,"v=1 first=%llu last=%llu draw_ns=%llu tagged_ns=%llu remaining_ns=%llu coverage=%15s clock_reads=%llu flags=%llu%n",&a,&b,&c,&d,&e,coverage,&f,&g,&used))==8&&used>0&&!s[used]){
                if(header||!armed||a!=detailFirst||b<a||strcmp(coverage,"OK")||g||d>c||e!=c-d)invalid=true;
                header=true;detailLast=b;draw=c;tagged=d;remaining=e;clockReads=f;flags=g;
            }else if((used=0,sscanf(s,"part=%31s calls=%llu exclusive_ns=%llu%n",name,&a,&b,&used))==3&&used>0&&!s[used]){
                static const char *names[]={"writeback","vertex_prepare","remaining"};
                unsigned k=0;for(;k<3;++k)if(!strcmp(name,names[k]))break;
                if(k==3||!header||(k<3&&(parts&(1u<<k))))invalid=true;
                else{parts|=1u<<k;if(k==2){if(a)invalid=true;remainingPart=b;}else if(!add(taggedSum,b)||!add(taggedCalls,a))invalid=true;}
            }else if((used=0,sscanf(s,"counters storage_ranges=%llu images_visited=%llu writeback_bytes=%llu index_expand_calls=%llu index_expand_bytes=%llu%n",&a,&b,&c,&d,&e,&used))==5&&used>0&&!s[used]){
                if(counters||!header||e%2||((d==0)!=(e==0))||((a==0)!=(c==0)))invalid=true;counters=true;
                storageRanges=a;imagesVisited=b;writebackBytes=c;indexExpandCalls=d;indexExpandBytes=e;
            }else invalid=true;
        }
        if(header&&cpu.header&&(detailFirst!=cpu.first||detailLast!=cpu.last))invalid=true;
        if(lines==23&&!complete())invalid=true;
        return !invalid;
    }
};

// Used by the real marker poller. No marker means no request (default OFF).
enum XblLiveProfileMode { XblLiveNone=0,XblLiveCpu=1,XblLiveDescriptor=2,XblLiveDraw=4,XblLiveCollision=8 };
inline XblLiveProfileMode XblChooseLiveProfile(bool attempted,bool cpu,bool descriptor,bool draw){
    if(attempted)return XblLiveNone;
    const unsigned mask=(cpu?1u:0u)|(descriptor?2u:0u)|(draw?4u:0u);
    if(mask&&(mask&(mask-1)))return XblLiveCollision;
    return static_cast<XblLiveProfileMode>(mask);
}

struct Session {

    OHNativeWindow *window{};

    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};

    uint32_t family{};VkSurfaceKHR surface{};VkSwapchainKHR swap{};

    VkPhysicalDeviceFeatures enabled{};VkBool32 drawParameters{};

    bool mirrorClampEnabled=false;

    std::chrono::steady_clock::time_point lastPresent{},paceDeadline{};

    VkBuffer readback{};VkDeviceMemory readbackMemory{};VkDeviceSize readbackBytes{};

    VkCommandPool pool{};VkCommandBuffer command{};VkFence acquire{},done{};

    VkFormat format{};VkExtent2D extent{};VkPresentModeKHR mode{};

    std::vector<VkImage> images;std::vector<VkSemaphore> rendered;

    uint64_t windowGeneration=0,nativeWidth=0,nativeHeight=0;bool rebuildSwap=false,replaceSurface=false,blitPending=false;

    XblSwapBudget swapBudget;

    unsigned wsiResultLogs=0;

    struct RetiredSwap {VkSwapchainKHR handle;std::vector<VkSemaphore> semaphores;XblRetiredSwapProof proof;uint64_t bytes;};

    std::vector<RetiredSwap> retiredSwaps;

    XblPresentCompletion presentCompletion;

    uint64_t activeSwapBytes=0;

    std::vector<VkSurfaceKHR> retiredSurfaces;

    std::vector<OHNativeWindow*> retiredWindows;

    std::string shaderPath,logPath;std::mutex mutex;uint64_t frames=0;

    XblLiveCpuReceipt cpuReceipt;
    XblDescriptorReceiptV2 descReceipt;
    bool descReported=false;
    XblDrawReceiptV1 drawReceipt;
    bool drawReported=false;
    uint64_t cpuLastPoll=0,cpuGeneration=0;
    std::string cpuMarker()const{return logPath.substr(0,logPath.find_last_of('/'))+"/gfx-cpu-profile-once";}
    std::string descMarker()const{return logPath.substr(0,logPath.find_last_of('/'))+"/gfx-descriptor-profile-once";}
    std::string drawMarker()const{return logPath.substr(0,logPath.find_last_of('/'))+"/gfx-draw-profile-once";}
    bool cpuPending()const{return cpuReceipt.owned||descReceipt.cpu.owned||drawReceipt.cpu.owned||access(cpuMarker().c_str(),F_OK)==0||access(descMarker().c_str(),F_OK)==0||access(drawMarker().c_str(),F_OK)==0;}
    bool otherMarkersPending()const{
        const std::string dir=logPath.substr(0,logPath.find_last_of('/'));
        return access((dir+"/gfx-performance-once").c_str(),F_OK)==0||access((dir+"/gfx-provenance-once").c_str(),F_OK)==0;
    }
    void pollCpuProfile(){
        const uint64_t now=perfClock();
        if(cpuReceipt.attempted||descReceipt.cpu.attempted||drawReceipt.cpu.attempted||(cpuLastPoll&&now-cpuLastPoll<1000000000ULL))return;
        cpuLastPoll=now;
        const bool oldPending=access(cpuMarker().c_str(),F_OK)==0,detailPending=access(descMarker().c_str(),F_OK)==0,drawPending=access(drawMarker().c_str(),F_OK)==0;
        const auto mode=XblChooseLiveProfile(false,oldPending,detailPending,drawPending);
        if(mode==XblLiveNone)return;
        if(mode==XblLiveCollision){
            if(oldPending)unlink(cpuMarker().c_str());if(detailPending)unlink(descMarker().c_str());if(drawPending)unlink(drawMarker().c_str());
            cpuReceipt.attempted=true;descReceipt.cpu.attempted=true;drawReceipt.cpu.attempted=true;
            log(drawPending?"hostDrawProfile REFUSED reason=MARKER_COLLISION once=1 noRetry=1":"hostDescriptorProfile REFUSED reason=MARKER_COLLISION once=1 noRetry=1");return;
        }
        const std::string marker=drawPending?drawMarker():(detailPending?descMarker():cpuMarker());
        if(unlink(marker.c_str()))return;
        cpuGeneration=windowGeneration;
        const char *prefix=drawPending?"hostDrawProfile":(detailPending?"hostDescriptorProfile":"hostCpuProfile");
        if(perf.active||provenance.active||otherMarkersPending()||GameDiagnosticsEnabled()){
            cpuReceipt.attempted=true;descReceipt.cpu.attempted=true;drawReceipt.cpu.attempted=true;
            char line[256];snprintf(line,sizeof(line),"%s REFUSED reason=FRONTEND_BUSY once=1 noRetry=1",prefix);log(line);return;
        }
        if(drawPending)drawReceipt.begin(60);else if(detailPending)descReceipt.begin(60);else cpuReceipt.begin(60); // Own BEFORE synchronous ARM.
        char line[512];snprintf(line,sizeof(line),"%s REQUEST pid=%d monotonic_ns=%llu generation=%llu native=%llux%llu maxFrames=60 maxSeconds=60 completion=NEXT_PRESENT noKill=1 diagnostics=OFF mode=%s",
            prefix,getpid(),(unsigned long long)now,(unsigned long long)windowGeneration,(unsigned long long)nativeWidth,(unsigned long long)nativeHeight,drawPending?"DRAW":(detailPending?"DESCRIPTOR":"CPU"));log(line);
        const int rc=drawPending?xbg_game_profile_draw_live(60):(detailPending?xbg_game_profile_descriptor_live(60):xbg_game_profile_cpu_live(60));
        if(rc){
            if(drawPending)drawReceipt.refused();else if(detailPending)descReceipt.refused();else cpuReceipt.refused();
            snprintf(line,sizeof(line),"%s REFUSED reason=BACKEND_BUSY once=1 noRetry=1",prefix);log(line);
        }
    }
    bool receiveCpuLog(const char *message){
        if(!message)return false;
        const bool main=!strncmp(message,"NVN CPU_TIMING ",15),detail=!strncmp(message,"NVN DESC_TIMING ",16);
        const bool drawDetail=!strncmp(message,"NVN DRAW_TIMING ",16);
        if(drawReceipt.cpu.owned&&(main||drawDetail)){
            const unsigned before=drawReceipt.lines;
            drawReceipt.accept(message);
            if(drawReceipt.lines>before)log(message);
            if(drawReceipt.complete()){
                char line[512];snprintf(line,sizeof(line),"hostDrawProfile END pid=%d monotonic_ns=%llu lines=%u bytes=%u first=%llu last=%llu frames=%llu reason=%s stableGeneration=%d coverage=OK scope=HOST_WALL_NOT_GPU_NOT_THREAD_CPU",
                    getpid(),(unsigned long long)perfClock(),drawReceipt.lines,drawReceipt.bytes,(unsigned long long)drawReceipt.cpu.first,(unsigned long long)drawReceipt.cpu.last,(unsigned long long)drawReceipt.cpu.frames,drawReceipt.cpu.reason,cpuGeneration==windowGeneration);
                if(strlen(line)+1>16384-drawReceipt.bytes){
                    drawReceipt.truncated=true;drawReported=true;log("hostDrawProfile RECEIPT_TRUNCATED incomplete=1 noRetry=1 noKill=1");return true;
                }
                log(line);
                drawReceipt.cpu.owned=false;
            }else if((drawReceipt.invalid||drawReceipt.truncated)&&!drawReported){
                log(drawReceipt.truncated?"hostDrawProfile RECEIPT_TRUNCATED incomplete=1 noRetry=1 noKill=1":"hostDrawProfile RECEIPT_INVALID incomplete=1 noRetry=1 noKill=1");drawReported=true;
            }
            return true;
        }
        if(descReceipt.cpu.owned&&(main||detail)){
            const unsigned before=descReceipt.lines;
            descReceipt.accept(message);
            if(descReceipt.lines>before)log(message); // Bounded malformed rows retained as evidence.
            if(descReceipt.complete()){
                char line[512];snprintf(line,sizeof(line),"hostDescriptorProfile END pid=%d monotonic_ns=%llu lines=%u bytes=%u first=%llu last=%llu frames=%llu reason=%s stableGeneration=%d coverage=OK scope=HOST_WALL_NOT_GPU_NOT_THREAD_CPU",
                    getpid(),(unsigned long long)perfClock(),descReceipt.lines,descReceipt.bytes,(unsigned long long)descReceipt.cpu.first,(unsigned long long)descReceipt.cpu.last,(unsigned long long)descReceipt.cpu.frames,descReceipt.cpu.reason,cpuGeneration==windowGeneration);log(line);
                descReceipt.cpu.owned=false;
            }else if((descReceipt.invalid||descReceipt.truncated)&&!descReported){
                log(descReceipt.truncated?"hostDescriptorProfile RECEIPT_TRUNCATED incomplete=1 noRetry=1 noKill=1":"hostDescriptorProfile RECEIPT_INVALID incomplete=1 noRetry=1 noKill=1");descReported=true;
            }
            return true;
        }
        if(!cpuReceipt.owned||!main)return false;
        const bool accepted=cpuReceipt.accept(message);
        if(accepted)log(message);
        if(cpuReceipt.complete()){
            char line[512];snprintf(line,sizeof(line),"hostCpuProfile END pid=%d monotonic_ns=%llu lines=%u bytes=%u first=%llu last=%llu frames=%llu reason=%s stableGeneration=%d scope=HOST_WALL_NOT_GPU_NOT_THREAD_CPU",
                getpid(),(unsigned long long)perfClock(),cpuReceipt.lines,cpuReceipt.bytes,(unsigned long long)cpuReceipt.first,(unsigned long long)cpuReceipt.last,(unsigned long long)cpuReceipt.frames,cpuReceipt.reason,cpuGeneration==windowGeneration);log(line);
            cpuReceipt.owned=false;
        }else if(cpuReceipt.truncated&&cpuReceipt.lines<21){
            log("hostCpuProfile RECEIPT_TRUNCATED incomplete=1 noRetry=1 noKill=1");cpuReceipt.lines=21;
        }
        return true;
    }
    XblPresentTimingOnce perf;
    xbg_game_stats perfStatsBefore{};
    uint64_t perfLastPoll=0;uint32_t perfSourceWidth=0,perfSourceHeight=0;int perfInterval=0;
    static uint64_t perfClock(){return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    uint64_t perfBeginPart(){return perf.active?perfClock():0;}
    void perfEndPart(XblPresentTimingOnce::Part part,uint64_t start){if(perf.active)perf.add(part,start,perfClock());}
    void perfEnter(uint32_t sourceWidth,uint32_t sourceHeight,int interval){
        const uint64_t now=perfClock();
        if(!cpuPending() && !provenance.active && access(provenanceMarker.c_str(),F_OK)!=0 && !perf.attempted && (!perfLastPoll || now-perfLastPoll>=1000000000ULL)){
            perfLastPoll=now;
            const std::string marker=logPath.substr(0,logPath.find_last_of('/'))+"/gfx-performance-once";
            if(unlink(marker.c_str())==0 && perf.arm(now,frames,windowGeneration)){
                perfSourceWidth=sourceWidth;perfSourceHeight=sourceHeight;perfInterval=interval;
                xbg_game_get_stats(&perfStatsBefore);
                log("hostPerf ARM maxCallbacks=64 maxSeconds=20 completion=NEXT_CALLBACK noGpuQuery=1 noReadback=1 noInput=1 noCpuProfile=1");
            }
        }
        if(perf.active&&(sourceWidth!=perfSourceWidth||sourceHeight!=perfSourceHeight||interval!=perfInterval))perf.stable=false;
        perf.enter(now);
    }
    void perfLeave(int rc){
        const uint64_t now=perfClock();
        if(!perf.leave(now,rc,frames,windowGeneration))return;
        xbg_game_stats after{};xbg_game_get_stats(&after);
        char line[2048];
        snprintf(line,sizeof(line),"hostPerf END reason=%d rc=%d stableGeneration=%d window_ns=%llu callbacks=%llu submitted=%llu callback_ns=%llu callback_max_ns=%llu outside_ns=%llu outside_max_ns=%llu source=%ux%u native=%llux%llu intervalMode=%d requestedInterval=%d submissions=%llu draws=%llu dispatches=%llu backendPresents=%llu shaderLoads=%llu scope=HOST_ELAPSED_NOT_GPU_NOT_DISPLAY completion=CALLBACK_BOUND",
            perf.reason,rc,perf.stable?1:0,(unsigned long long)(now-perf.began),(unsigned long long)perf.callbacks,
            (unsigned long long)(perf.end_frames-perf.start_frames),(unsigned long long)perf.callback_ns,(unsigned long long)perf.callback_max_ns,
            (unsigned long long)perf.outside_ns,(unsigned long long)perf.outside_max_ns,
            perfSourceWidth,perfSourceHeight,(unsigned long long)nativeWidth,(unsigned long long)nativeHeight,int(mode),perfInterval,
            (unsigned long long)(after.submissions-perfStatsBefore.submissions),(unsigned long long)(after.draws-perfStatsBefore.draws),
            (unsigned long long)(after.dispatches-perfStatsBefore.dispatches),(unsigned long long)(after.presents-perfStatsBefore.presents),
            (unsigned long long)(after.shader_loads-perfStatsBefore.shader_loads));log(line);
        snprintf(line,sizeof(line),"hostPerf PARTS acquireCall_ns=%llu acquireFence_ns=%llu blitFence_ns=%llu pacing_ns=%llu queuePresent_ns=%llu acquireCall_max=%llu acquireFence_max=%llu blitFence_max=%llu pacing_max=%llu queuePresent_max=%llu acquireCalls=%llu acquireFenceCalls=%llu blitFenceCalls=%llu pacingCalls=%llu queuePresentCalls=%llu pacing=INSIDE_CALLBACK outside=BETWEEN_CALLBACKS_NO_PRESENT_CALLBACK noCallbackTimeout=EXTERNAL_WATCH_ONLY_NO_KILL",
            (unsigned long long)perf.part[0].total_ns,(unsigned long long)perf.part[1].total_ns,(unsigned long long)perf.part[2].total_ns,
            (unsigned long long)perf.part[3].total_ns,(unsigned long long)perf.part[4].total_ns,
            (unsigned long long)perf.part[0].max_ns,(unsigned long long)perf.part[1].max_ns,(unsigned long long)perf.part[2].max_ns,
            (unsigned long long)perf.part[3].max_ns,(unsigned long long)perf.part[4].max_ns,
            (unsigned long long)perf.part[0].calls,(unsigned long long)perf.part[1].calls,(unsigned long long)perf.part[2].calls,
            (unsigned long long)perf.part[3].calls,(unsigned long long)perf.part[4].calls);log(line);
    }
    XblProvenanceOnce provenance;

    std::string provenanceMarker,provenanceBase;

    void pollProvenance(){
        const std::string dir=logPath.substr(0,logPath.find_last_of('/'));
        if(cpuPending()||perf.active||access((dir+"/gfx-performance-once").c_str(),F_OK)==0)return;

        const uint64_t now=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());

        provenance.poll(now,[&]{return unlink(provenanceMarker.c_str())==0;},

            [&]{const std::string path=provenanceBase+std::to_string(getpid())+"-"+std::to_string(now)+".txt";

                log(("provenancePath="+path+" exclusiveCreate=1 externalWatchdogSeconds=20 changedVariables=SEE_PACKAGE_MANIFEST").c_str());

                return xbg_game_provenance(path.c_str());},

            [&]{return xbg_game_provenance(nullptr);},

            [&](const char *event,int rc,uint64_t requested){char line[256];

                const auto returned=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

                snprintf(line,sizeof(line),"provenance event=%s rc=%d callback_ns=%llu returned_ns=%llu",event,rc,(unsigned long long)requested,(unsigned long long)returned);log(line);});

    }



    void log(const char *text){FILE *f=fopen(logPath.c_str(),"ab");if(f){fprintf(f,"%s\n",text);fclose(f);}}

    void check(VkResult r,const char *what){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(what)+" VkResult="+std::to_string(r));}

    void need(bool ok,const char *what){if(!ok)throw std::runtime_error(what);}

    void auditCapabilities(){

        // Inventory the complete compressed family and conversion destinations once,

        // before the guest can stop at its first unsupported texture.

        std::string path=logPath.substr(0,logPath.find_last_of('/'))+"/graphics-capabilities.txt";

        FILE *f=fopen(path.c_str(),"wb");if(!f)return;

        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};

        VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&sg;vkGetPhysicalDeviceProperties2(physical,&p);

        fprintf(f,"device=%s api=%u BC=%u ASTC=%u gather=%u storageWriteWithoutFormat=%u tessellation=%u drawParameters=%u\n",

            p.properties.deviceName,p.properties.apiVersion,enabled.textureCompressionBC,enabled.textureCompressionASTC_LDR,

            enabled.shaderImageGatherExtended,enabled.shaderStorageImageWriteWithoutFormat,enabled.tessellationShader,drawParameters);

        fprintf(f,"subgroupSize=%u subgroupStages=0x%x subgroupOperations=0x%x\n",sg.subgroupSize,sg.supportedStages,sg.supportedOperations);

        const VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_R16_SNORM,

            VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT,

            VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC2_UNORM_BLOCK,

            VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC4_UNORM_BLOCK,VK_FORMAT_BC4_SNORM_BLOCK,

            VK_FORMAT_BC5_UNORM_BLOCK,VK_FORMAT_BC5_SNORM_BLOCK,VK_FORMAT_BC6H_UFLOAT_BLOCK,

            VK_FORMAT_BC6H_SFLOAT_BLOCK,VK_FORMAT_BC7_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK,

            VK_FORMAT_ASTC_4x4_UNORM_BLOCK,VK_FORMAT_ASTC_4x4_SRGB_BLOCK,

            VK_FORMAT_D16_UNORM,VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_D32_SFLOAT_S8_UINT};

        for(auto fmt:formats){

            VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(physical,fmt,&fp);

            VkImageFormatProperties ip{};VkResult r=vkGetPhysicalDeviceImageFormatProperties(physical,fmt,VK_IMAGE_TYPE_2D,

                VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&ip);

            fprintf(f,"format=%u optimal=0x%x sampledUploadResult=%d max2D=%ux%u samples=0x%x\n",

                unsigned(fmt),fp.optimalTilingFeatures,int(r),ip.maxExtent.width,ip.maxExtent.height,ip.sampleCounts);

        }

        fclose(f);

    }

    void init(){

        window=AcquireGameSurface();need(window,"Game surface unavailable; keep application foreground");

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="XblGame";app.apiVersion=VK_API_VERSION_1_1;

        const char *ext[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_OHOS_SURFACE_EXTENSION_NAME};

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;ci.enabledExtensionCount=2;ci.ppEnabledExtensionNames=ext;

        check(vkCreateInstance(&ci,nullptr,&instance),"vkCreateInstance");

        auto surfaceFn=reinterpret_cast<PFN_vkCreateSurfaceOHOS>(vkGetInstanceProcAddr(instance,"vkCreateSurfaceOHOS"));need(surfaceFn,"OHOS surface extension missing");

        VkSurfaceCreateInfoOHOS sc{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};sc.window=window;check(surfaceFn(instance,&sc,nullptr,&surface),"surface");

        uint32_t n=0;check(vkEnumeratePhysicalDevices(instance,&n,nullptr),"physical count");need(n>0&&n<=16,"physical count invalid");

        std::vector<VkPhysicalDevice> devices(n);check(vkEnumeratePhysicalDevices(instance,&n,devices.data()),"physical devices");physical=devices[0];

        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);need(props.apiVersion>=VK_API_VERSION_1_1,"Vulkan 1.1 required");

        VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};

        VkPhysicalDeviceFeatures2 fs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};fs.pNext=&draw;vkGetPhysicalDeviceFeatures2(physical,&fs);

        enabled=fs.features;drawParameters=draw.shaderDrawParameters;

        auditCapabilities();

        uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,nullptr);need(nq>0&&nq<=64,"queue count invalid");

        std::vector<VkQueueFamilyProperties> qs(nq);vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,qs.data());family=UINT32_MAX;

        for(uint32_t i=0;i<nq;i++){VkBool32 present=0;check(vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&present),"present support");if(present&&qs[i].queueCount&&(qs[i].queueFlags&3)==3){family=i;break;}}

        need(family!=UINT32_MAX,"No graphics compute present queue");float priority=1;

        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;

        uint32_t extensionCount=0;check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extensionCount,nullptr),"extension count");

        need(extensionCount<=4096,"Invalid device extension count");std::vector<VkExtensionProperties> extensions(extensionCount);

        check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extensionCount,extensions.data()),"device extensions");

        bool mirrorSupported=false;for(const auto &e:extensions)if(!strcmp(e.extensionName,VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME))mirrorSupported=true;

        std::vector<const char*> deviceExt{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

        // This host requests Vulkan 1.1: explicitly enable the KHR extension,

        // rather than treating the physical device's newer version as enabled.

        if(mirrorSupported)deviceExt.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
        // Zero-copy memory pools need exportable memory fds (VK_KHR_external_memory_fd) when present.
        {uint32_t n=0;vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr);std::vector<VkExtensionProperties> ext(n);vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,ext.data());
         for(auto &e:ext)if(!strcmp(e.extensionName,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)){deviceExt.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);break;}}

        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.pNext=&draw;di.pEnabledFeatures=&enabled;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.enabledExtensionCount=static_cast<uint32_t>(deviceExt.size());di.ppEnabledExtensionNames=deviceExt.data();

        check(vkCreateDevice(physical,&di,nullptr,&device),"device");vkGetDeviceQueue(device,family,0,&queue);

        mirrorClampEnabled=mirrorSupported;log(("enabledSamplerMirrorClampToEdge="+std::to_string(mirrorClampEnabled)+" path=Vulkan1.1KHR").c_str());

        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;check(vkCreateCommandPool(device,&cp,nullptr,&pool),"command pool");

        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;check(vkAllocateCommandBuffers(device,&ca,&command),"command buffer");

        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(device,&fi,nullptr,&acquire),"acquire fence");check(vkCreateFence(device,&fi,nullptr,&done),"draw fence");

        log((std::string("device=")+props.deviceName+" enabledInt64="+std::to_string(enabled.shaderInt64)+" enabledTessellation="+std::to_string(enabled.tessellationShader)+" enabledDrawParameters="+std::to_string(drawParameters)).c_str());

    }

    VkSwapchainKHR retireSwap(){

        const VkSwapchainKHR old=swap;

        if(old){retiredSwaps.push_back({old,std::move(rendered),{(uint64_t)(uintptr_t)surface,0},activeSwapBytes});images.clear();swap=VK_NULL_HANDLE;activeSwapBytes=0;presentCompletion.reset();}

        return old;

    }

    void collectCompletedSwaps(unsigned acquiredIndex){

        if(!presentCompletion.acquired(acquiredIndex,true))return;

        unsigned released=0;

        for(auto it=retiredSwaps.begin();it!=retiredSwaps.end();){

            if(!it->proof.ready((uint64_t)(uintptr_t)swap)){++it;continue;}

            // Same-surface oldSwapchain chain. A waited acquire fence for the

            // new chain's first present proves the old chain no longer presents.

            // https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html

            for(auto semaphore:it->semaphores)vkDestroySemaphore(device,semaphore,nullptr);

            vkDestroySwapchainKHR(device,it->handle,nullptr);

            need(swapBudget.release(it->bytes),"Swapchain retirement accounting invalid");

            it=retiredSwaps.erase(it);++released;

        }

        if(released){char line[256];snprintf(line,sizeof(line),"wsiCollect frame=%llu proof=first-present-image-reacquired index=%u released=%u retained=%zu budgetGenerations=%u bytes=%llu",(unsigned long long)frames,acquiredIndex,released,retiredSwaps.size(),swapBudget.generations,swapBudget.imageByteLowerBound);log(line);}

    }

    bool synchronizeSurface(int interval){

        GameSurfaceSnapshot next{};

        need(WaitForGameSurface(next),"Game presentation cancelled or native window reference failed");

        const bool changed=next.window!=window;

        const bool resized=next.generation!=windowGeneration;

        if(changed||replaceSurface){

            retireSwap();

            retiredSurfaces.push_back(surface);

            if(changed){retiredWindows.push_back(window);window=next.window;}

            else OH_NativeWindow_NativeObjectUnreference(next.window);

            auto create=reinterpret_cast<PFN_vkCreateSurfaceOHOS>(vkGetInstanceProcAddr(instance,"vkCreateSurfaceOHOS"));

            need(create,"OHOS surface extension missing during recovery");

            VkSurfaceCreateInfoOHOS info{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};info.window=window;

            check(create(instance,&info,nullptr,&surface),"replacement surface");

            VkBool32 supported=VK_FALSE;check(vkGetPhysicalDeviceSurfaceSupportKHR(physical,family,surface,&supported),"replacement surface support");

            need(supported,"Replacement surface requires a different queue");

            replaceSurface=false;rebuildSwap=true;

        }else OH_NativeWindow_NativeObjectUnreference(next.window);

        windowGeneration=next.generation;

        nativeWidth=next.width;nativeHeight=next.height;

        const VkPresentModeKHR desired=interval>0?VK_PRESENT_MODE_FIFO_KHR:VK_PRESENT_MODE_IMMEDIATE_KHR;

        if(!swap||rebuildSwap||resized||mode!=desired){

            char reason[384];snprintf(reason,sizeof(reason),"wsiRebuild frame=%llu generation=%llu identity=%d resized=%d forced=%d replace=%d mode=%d/%d native=%llux%llu budgetGenerations=%u bytes=%llu",(unsigned long long)frames,(unsigned long long)next.generation,changed,resized,rebuildSwap,replaceSurface,int(mode),int(desired),(unsigned long long)nativeWidth,(unsigned long long)nativeHeight,swapBudget.generations,swapBudget.imageByteLowerBound);log(reason);

            const VkSwapchainKHR old=retireSwap();

            if(!createSwap(interval,old))return false;rebuildSwap=false;

        }

        return true;

    }

    bool createSwap(int interval,VkSwapchainKHR old=VK_NULL_HANDLE){

        need(interval>=0&&interval<=5,"Invalid presentation interval");

        mode=interval>0?VK_PRESENT_MODE_FIFO_KHR:VK_PRESENT_MODE_IMMEDIATE_KHR;

        uint32_t count=0;check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&count,nullptr),"mode count");need(count>0&&count<=64,"mode count invalid");std::vector<VkPresentModeKHR> modes(count);check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&count,modes.data()),"modes");need(std::find(modes.begin(),modes.end(),mode)!=modes.end(),"Requested present mode unsupported");

        VkSurfaceCapabilitiesKHR caps{};

        for(;;){

            const VkResult queried=vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps);

            if(queried==VK_ERROR_SURFACE_LOST_KHR){replaceSurface=true;return false;}

            check(queried,"surface caps");

            const auto plan=XblSelectSurfaceExtent(caps.currentExtent.width,caps.currentExtent.height,

                caps.minImageExtent.width,caps.minImageExtent.height,caps.maxImageExtent.width,caps.maxImageExtent.height,nativeWidth,nativeHeight);

            need(plan.status>=0,"Invalid surface extent bounds");

            if(plan.status>0){extent={plan.width,plan.height};break;}

            GameSurfaceSnapshot refreshed{};

            need(WaitForGameSurfaceRefresh(windowGeneration,refreshed),"Game presentation cancelled while waiting for surface extent");

            const bool replaced=refreshed.window!=window;

            windowGeneration=refreshed.generation;nativeWidth=refreshed.width;nativeHeight=refreshed.height;

            OH_NativeWindow_NativeObjectUnreference(refreshed.window);

            if(replaced)return false;

        }

        char capsLine[256];snprintf(capsLine,sizeof(capsLine),"wsiCaps frame=%llu extent=%ux%u current=%ux%u transform=%u supportedTransforms=%u",(unsigned long long)frames,extent.width,extent.height,caps.currentExtent.width,caps.currentExtent.height,unsigned(caps.currentTransform),unsigned(caps.supportedTransforms));log(capsLine);

        need(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT,"Surface lacks transfer destination");

        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"format count");need(count>0&&count<=128,"format count invalid");std::vector<VkSurfaceFormatKHR> formats(count);check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"formats");VkSurfaceFormatKHR chosen{};

        for(auto f:formats)if((f.format==VK_FORMAT_R8G8B8A8_UNORM||f.format==VK_FORMAT_B8G8R8A8_UNORM)&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR){chosen=f;break;}

        need(chosen.format!=VK_FORMAT_UNDEFINED,"No supported surface format");format=chosen.format;

        uint32_t imagesWanted=caps.minImageCount+1;if(caps.maxImageCount)imagesWanted=std::min(imagesWanted,caps.maxImageCount);need(imagesWanted<=8,"Too many swapchain images");

        need(swapBudget.allows(extent.width,extent.height,imagesWanted),"Swapchain recovery budget exhausted; restart application (image bytes are a lower bound)");

        VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};si.surface=surface;si.minImageCount=imagesWanted;si.imageFormat=format;si.imageColorSpace=chosen.colorSpace;si.imageExtent=extent;si.imageArrayLayers=1;si.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;si.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;const unsigned upright=XblUnrotatedPresentTransform(unsigned(caps.supportedTransforms));need(upright!=0,"Surface lacks identity transform; explicit source pre-rotation required");si.preTransform=static_cast<VkSurfaceTransformFlagBitsKHR>(upright);

        {char transformLine[160];snprintf(transformLine,sizeof(transformLine),"wsiTransform current=%u selected=%u sourcePreRotated=0",unsigned(caps.currentTransform),upright);log(transformLine);}si.compositeAlpha=static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));si.presentMode=mode;si.clipped=VK_TRUE;

        si.oldSwapchain=old;

        check(vkCreateSwapchainKHR(device,&si,nullptr,&swap),"swapchain");check(vkGetSwapchainImagesKHR(device,swap,&count,nullptr),"image count");need(count>0&&count<=8,"swapchain image count invalid");

        need(swapBudget.allows(extent.width,extent.height,count),"Actual swapchain image estimate exceeds recovery budget");swapBudget.record(extent.width,extent.height,count);

        activeSwapBytes=uint64_t(extent.width)*extent.height*4*count;

        presentCompletion.reset();

        // All prior pending links of this same surface now depend on completion

        // of this replacement. Different surfaces stay retained and bounded.

        if(old)for(auto &retired:retiredSwaps)retired.proof.replacement((uint64_t)(uintptr_t)surface,(uint64_t)(uintptr_t)swap);

        images.resize(count);rendered.resize(count);check(vkGetSwapchainImagesKHR(device,swap,&count,images.data()),"images");

        VkSemaphoreCreateInfo sm{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};for(auto &s:rendered)check(vkCreateSemaphore(device,&sm,nullptr,&s),"render semaphore");

        return true;

    }

    void prepareReadback(uint32_t width,uint32_t height){

        VkDeviceSize bytes=uint64_t(width)*height*4;

        need(bytes>0 && bytes<=64*1024*1024,"Diagnostic source size invalid");

        if(readback){need(bytes<=readbackBytes,"Diagnostic source resized");return;}

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;

        check(vkCreateBuffer(device,&bi,nullptr,&readback),"readback buffer");

        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,readback,&req);

        VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);uint32_t type=UINT32_MAX;

        for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){type=i;if(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;}

        need(type!=UINT32_MAX,"No host visible readback memory");VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;

        check(vkAllocateMemory(device,&ai,nullptr,&readbackMemory),"readback allocate");check(vkBindBufferMemory(device,readback,readbackMemory,0),"readback bind");readbackBytes=bytes;

    }

    void saveReadback(uint32_t width,uint32_t height,VkFormat fmt){

        void *mapped=nullptr;check(vkMapMemory(device,readbackMemory,0,VK_WHOLE_SIZE,0,&mapped),"readback map");

        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=readbackMemory;range.size=VK_WHOLE_SIZE;

        VkResult result=vkInvalidateMappedMemoryRanges(device,1,&range);if(result!=VK_SUCCESS){vkUnmapMemory(device,readbackMemory);check(result,"readback invalidate");}

        const auto *p=static_cast<const uint8_t*>(mapped);uint64_t nonzero=0,opaque=0;unsigned min[4]={255,255,255,255},max[4]={0,0,0,0};

        for(uint64_t i=0;i<uint64_t(width)*height;i++){if(p[i*4]||p[i*4+1]||p[i*4+2])++nonzero;if(p[i*4+3]==255)++opaque;for(unsigned c=0;c<4;c++){min[c]=std::min(min[c],unsigned(p[i*4+c]));max[c]=std::max(max[c],unsigned(p[i*4+c]));}}

        char line[512];snprintf(line,sizeof(line),"sourceReadback frame=%llu size=%ux%u format=%u nonzeroRGB=%llu opaque=%llu min=%u,%u,%u,%u max=%u,%u,%u,%u",static_cast<unsigned long long>(frames+1),width,height,unsigned(fmt),static_cast<unsigned long long>(nonzero),static_cast<unsigned long long>(opaque),min[0],min[1],min[2],min[3],max[0],max[1],max[2],max[3]);log(line);

        std::string path=logPath.substr(0,logPath.find_last_of('/'))+"/source-frame-"+std::to_string(frames+1)+".rgba";

        FILE *f=fopen(path.c_str(),"wb");if(f){size_t written=fwrite(mapped,1,size_t(width)*height*4,f);fclose(f);if(written!=size_t(width)*height*4)log("sourceReadback file short write");}else log("sourceReadback file open failed");

        vkUnmapMemory(device,readbackMemory);

    }

    int present(VkImage source,VkFormat srcFormat,uint32_t width,uint32_t height,const int32_t crop[4],int interval){

        std::lock_guard<std::mutex> guard(mutex);

        try{

            // The previous blit may still run; it reuses `command`/`done`, so wait here
            // (one frame later) instead of right after submitting. CPU work of the
            // next game frame then overlaps GPU work of this one.
            if(blitPending){check(vkWaitForFences(device,1,&done,VK_TRUE,5000000000ULL),"previous blit completion");blitPending=false;}
            for(unsigned attempt=0;attempt<2;++attempt){

            if(!synchronizeSurface(interval))continue;

            if(frames==0){char line[256];snprintf(line,sizeof(line),"firstPresent source=%ux%u format=%u interval=%d crop=%d,%d,%d,%d",width,height,unsigned(srcFormat),interval,crop?crop[0]:0,crop?crop[1]:0,crop?crop[2]:0,crop?crop[3]:0);log(line);}

            need((interval==0&&mode==VK_PRESENT_MODE_IMMEDIATE_KHR)||(interval>0&&interval<=5&&mode==VK_PRESENT_MODE_FIFO_KHR),"Invalid presentation interval");

            int32_t x=0,y=0,w=width,h=height;if(crop&&(crop[0]||crop[1]||crop[2]||crop[3])){x=crop[0];y=crop[1];w=crop[2];h=crop[3];}

            need(x>=0&&y>=0&&w>0&&h>0&&uint64_t(x)+w<=width&&uint64_t(y)+h<=height,"Invalid presentation crop");

            VkFormatProperties a{},b{};vkGetPhysicalDeviceFormatProperties(physical,srcFormat,&a);vkGetPhysicalDeviceFormatProperties(physical,format,&b);need((a.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)&&(b.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT),"Image formats cannot blit");

            check(vkResetFences(device,1,&acquire),"reset acquire");uint32_t index=0;uint64_t perfAcquire=perfBeginPart();VkResult r=vkAcquireNextImageKHR(device,swap,1000000000ULL,VK_NULL_HANDLE,acquire,&index);perfEndPart(XblPresentTimingOnce::AcquireCall,perfAcquire);

            if(r!=VK_SUCCESS && wsiResultLogs++<16){char result[160];snprintf(result,sizeof(result),"wsiResult frame=%llu op=acquire VkResult=%d",(unsigned long long)frames,int(r));log(result);}

            if(r==VK_ERROR_OUT_OF_DATE_KHR||r==VK_ERROR_SURFACE_LOST_KHR){rebuildSwap=true;replaceSurface=r==VK_ERROR_SURFACE_LOST_KHR;continue;}

            if(r!=VK_SUCCESS&&r!=VK_SUBOPTIMAL_KHR)check(r,"Acquire failed or surface changed");

            rebuildSwap=r==VK_SUBOPTIMAL_KHR;

            uint64_t perfAcquireFence=perfBeginPart();VkResult perfAcquireResult=vkWaitForFences(device,1,&acquire,VK_TRUE,5000000000ULL);perfEndPart(XblPresentTimingOnce::AcquireFence,perfAcquireFence);check(perfAcquireResult,"acquire wait");need(index<images.size(),"Acquired index invalid");

            collectCompletedSwaps(index);

            bool capture=XblSourceReadbackAllowed(GameDiagnosticsEnabled(),frames,srcFormat==VK_FORMAT_R8G8B8A8_UNORM||srcFormat==VK_FORMAT_B8G8R8A8_UNORM);if(capture)prepareReadback(width,height);

            check(vkResetFences(device,1,&done),"reset done");check(vkResetCommandBuffer(command,0),"reset commands");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(command,&begin),"begin");

            if(capture){VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};vkCmdCopyImageToBuffer(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback;host.size=VK_WHOLE_SIZE;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);}

            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);

            VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;blit.srcOffsets[0]={x,y,0};blit.srcOffsets[1]={x+w,y+h,1};blit.dstOffsets[1]={int32_t(extent.width),int32_t(extent.height),1};
            // Game image narrower than the screen (aspect capped below 2:1 on wide phones): centre it with black side bars, never stretch.
            {const float srcA=float(w)/float(h),dstA=float(extent.width)/float(extent.height);int32_t dx0=0,dx1=int32_t(extent.width);
             if(h>0&&srcA<dstA*0.99f){const int32_t cw=int32_t(float(extent.height)*srcA+0.5f);dx0=(int32_t(extent.width)-cw)/2;dx1=dx0+cw;
                VkClearColorValue black{};VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&all);
                VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,0,nullptr);}
             blit.dstOffsets[0]={dx0,0,0};blit.dstOffsets[1]={dx1,int32_t(extent.height),1};
             gContentX0.store(float(dx0)/float(extent.width));gContentW.store(float(dx1-dx0)/float(extent.width));}
            vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);

            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);check(vkEndCommandBuffer(command),"end");

            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&rendered[index];check(vkQueueSubmit(queue,1,&submit,done),"submit");blitPending=true;if(capture){uint64_t perfBlit=perfBeginPart();VkResult perfBlitResult=vkWaitForFences(device,1,&done,VK_TRUE,5000000000ULL);perfEndPart(XblPresentTimingOnce::BlitFence,perfBlit);check(perfBlitResult,"source blit completion");blitPending=false;}

            if(capture)saveReadback(width,height,srcFormat);

            // The original guest host game mode uses a 60 Hz timebase. Preserve the

            // requested interval with monotonic host pacing plus FIFO. This

            // limits submission cadence; it does not claim display timestamps.

            if(interval>0 && frames){

                // Fixed cadence: advance the deadline by one period instead of re-anchoring on the (blocking) present
                // return, so 30 Hz frames are evenly spaced; re-align only when more than a period behind.
                const auto period=std::chrono::nanoseconds(uint64_t(interval)*1000000000ULL/60);const auto nowPace=std::chrono::steady_clock::now();
                paceDeadline+=period;if(paceDeadline+period<nowPace || paceDeadline>nowPace+2*period)paceDeadline=nowPace;
                auto deadline=paceDeadline;

                uint64_t perfPace=perfBeginPart();std::this_thread::sleep_until(deadline);perfEndPart(XblPresentTimingOnce::Pacing,perfPace);

            }

            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&rendered[index];pi.swapchainCount=1;pi.pSwapchains=&swap;pi.pImageIndices=&index;uint64_t perfPresent=perfBeginPart();r=vkQueuePresentKHR(queue,&pi);perfEndPart(XblPresentTimingOnce::QueuePresent,perfPresent);

            if(r!=VK_SUCCESS && wsiResultLogs++<16){char result[160];snprintf(result,sizeof(result),"wsiResult frame=%llu op=present VkResult=%d",(unsigned long long)frames,int(r));log(result);}

            if(r==VK_ERROR_OUT_OF_DATE_KHR||r==VK_ERROR_SURFACE_LOST_KHR){rebuildSwap=true;replaceSurface=r==VK_ERROR_SURFACE_LOST_KHR;continue;}

            if(r!=VK_SUCCESS&&r!=VK_SUBOPTIMAL_KHR)check(r,"Present failed");

            presentCompletion.presented(index);

            rebuildSwap=rebuildSwap||r==VK_SUBOPTIMAL_KHR;

            lastPresent=std::chrono::steady_clock::now();

            ++frames;presentedFrames.store(frames);if(GameDiagnosticsEnabled()&&(frames<=3||frames%120==0))log(("gamePresentSubmitted="+std::to_string(frames)).c_str());return 0;

            }

            throw std::runtime_error("Presentation surface remained unavailable after bounded recovery");

        }catch(const std::exception &e){log(e.what());return -1;}

    }

};

Session *session=nullptr;

int Present(void *u,void *,VkImage image,VkFormat f,uint32_t w,uint32_t h,const int32_t crop[4],int32_t interval){auto *s=static_cast<Session*>(u);s->perfEnter(w,h,interval);const int rc=s->present(image,f,w,h,crop,interval);s->perfLeave(rc);s->pollCpuProfile();s->pollProvenance();return rc;}

void Log(void *u,const char *message){if(!message)return;if(XblGraphicsFatal(message)){static_cast<Session*>(u)->log(message);return;}if(static_cast<Session*>(u)->receiveCpuLog(message))return;if(GameDiagnosticsEnabled()||strstr(message,"NVN PERF")||strstr(message,"NVN TEXBUF")||strstr(message,"NVN async")||strstr(message,"NVN caps")||strstr(message,"NVN CPUGPU")||strstr(message,"NVN COPYT2T")||strstr(message,"NVN ASTC2BC")||strstr(message,"NVN wtrack")||strstr(message,"NVN GPASS")||strstr(message,"NVN warm")||strstr(message,"NVN noload")||strstr(message,"NVN NULLTEX")||strstr(message,"NVN pipeline cache")||strstr(message,"PROVENANCE_CLOSE")||strstr(message,"failed")||strstr(message,"FAIL")||strstr(message,"error"))static_cast<Session*>(u)->log(message);}

}

#endif

bool InitializeGameGraphics(const std::string &files,GuestInvoke invoke,GuestFatal fatal,std::string &report){

#ifdef XBL_GAME_GRAPHICS

    if(session){report="Game graphics already initialized; restart required";return false;}

    session=new Session;session->shaderPath=files+"/game-shaders";session->logPath=files+"/game-graphics.txt";session->provenanceMarker=files+"/gfx-provenance-once";session->provenanceBase=files+"/gfx-provenance-";

    try{session->init();xbg_game_config config{};config.size=sizeof(config);config.physical=session->physical;config.device=session->device;config.queue=session->queue;config.queue_family=session->family;config.enabled_core=session->enabled;config.enabled_shader_draw_parameters=session->drawParameters;config.fence_timeout_ns=5000000000ULL;config.shader_directory=session->shaderPath.c_str();config.userdata=session;config.invoke_guest=invoke;config.fatal=fatal;config.present=Present;config.log=Log;char error[1024]{};int rc=xbg_game_initialize(&config,error,sizeof(error));if(rc)throw std::runtime_error(error);if(xbg_game_set_sampler_mirror_clamp_to_edge(session->mirrorClampEnabled?VK_TRUE:VK_FALSE)!=0)throw std::runtime_error("Sampler capability registration failed");report="gameGraphics=READY";session->log(report.c_str());return true;}catch(const std::exception &e){report=e.what();session->log(report.c_str());return false;}

#else

    report="Game graphics not configured";return false;

#endif

}

void *ResolveGameGraphics(const char *name){

#ifdef XBL_GAME_GRAPHICS

    return xbg_nvn_get_proc_address(name);

#else

    return nullptr;

#endif

}

std::string GameGraphicsStats(){

#ifdef XBL_GAME_GRAPHICS

    // The integrated backend exposes atomic counters, safe after a fatal unwind.

    xbg_game_stats s{};xbg_game_get_stats(&s);

    return "gameStats submissions="+std::to_string(s.submissions)+" draws="+std::to_string(s.draws)+" dispatches="+std::to_string(s.dispatches)+" presents="+std::to_string(s.presents)+" shaderLoads="+std::to_string(s.shader_loads);

#else

    return "gameStats=UNAVAILABLE";

#endif

}

uint64_t GamePresentedFrames(){return presentedFrames.load();}

bool GameSurfacePaused(){return IsGameSurfaceWaiting();}
