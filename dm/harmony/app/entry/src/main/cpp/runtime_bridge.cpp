#include <window_manager/oh_display_manager.h>
#include "runtime_bridge.h"
#include "controller_bridge.h"
#include "game_touch.h"
#include "crash_recorder.h"
#include "game_graphics.h"
#include "game_keyboard.h"
#include "gpu_checks.h"
#include <pthread.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <sstream>
#include <memory>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <time.h>
#ifdef XBL_RUNTIME_EXECUTION
#include "xblr_execution.h"
static_assert(sizeof(xblr_thread_observation)==80,"Runtime thread row ABI mismatch");
static_assert(sizeof(xblr_thread_snapshot)==5144,"Runtime thread snapshot ABI mismatch");
static_assert(offsetof(xblr_thread_snapshot,threads)==24,"Runtime thread rows offset mismatch");
#endif
#ifdef XBL_GAME_GRAPHICS
#include "xbg_game.h"
#endif
namespace {
std::atomic<bool> started{false};
std::atomic<void*> runtimeHandle{nullptr};
// 3D render scale mode: files/scale-mode holds "quality" (native ~0.53) or "fast" (default, 0.45).
std::string gFilesDir;constexpr float kFastScale3d=0.45f;
bool ReadFastMode(){if(gFilesDir.empty())return true;FILE *f=fopen((gFilesDir+"/scale-mode").c_str(),"rb");if(!f)return true;char b[16]{};size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);return !(n>=7&&!strncmp(b,"quality",7));}
std::atomic<int> foregroundState{1};
std::atomic<uint64_t> focusEpoch{0};
std::atomic<uint64_t> diagnosticsStartedNs{0};
std::atomic<bool> diagnosticsEnabled{false};
std::atomic<bool> inputDiagnosticsEnabled{false};
std::atomic<bool> uiTouchReceiptPending{false};
std::atomic<bool> uiTouchDiagnosticsEnabled{false};
uint64_t MonotonicNs(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);}
struct Work {napi_async_work work{};napi_deferred deferred{};std::string path,report;uint32_t stage=2;uint64_t maxGates=200000;};
void Save(const Work &w,const std::string &text){FILE *f=fopen((w.path+"/runtime-startup.txt").c_str(),"ab");if(f){fwrite(text.data(),1,text.size(),f);fflush(f);fclose(f);}}
void *Thread(void *data){
    auto &w=*static_cast<Work *>(data);std::ostringstream out;
    const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    Save(w,"\nSESSION pid="+std::to_string(getpid())+" epoch_ms="+std::to_string(epoch)+"\n");
    int logfd=open((w.path+"/runtime-native.txt").c_str(),O_CREAT|O_WRONLY|O_APPEND,0600);
    if(logfd>=0){fflush(stderr);fflush(stdout);dup2(logfd,STDERR_FILENO);dup2(logfd,STDOUT_FILENO);close(logfd);setvbuf(stderr,nullptr,_IONBF,0);setvbuf(stdout,nullptr,_IONBF,0);}
    fprintf(stderr,"[frontend] SESSION pid=%d epoch_ms=%lld stage=%u\n",getpid(),static_cast<long long>(epoch),w.stage);
    Save(w,"runtime startup worker entered; requestedStage="+std::to_string(w.stage)+" maxGates="+std::to_string(w.maxGates)+" stackBytes=4194304\n");
#ifdef XBL_RUNTIME_EXECUTION
    // Consume a device-executor marker once. Runtime caches these flags at startup;
    // a fresh process without the marker must explicitly remove both variables.
    const std::string diagnosticsMarker=w.path+"/runtime-diagnostics-once";
    const bool diagnostics=w.stage==9 && unlink(diagnosticsMarker.c_str())==0;
    diagnosticsEnabled.store(diagnostics);
    inputDiagnosticsEnabled.store(w.stage==9 && unlink((w.path+"/input-receipt-once").c_str())==0);
    const bool uiReceipt=w.stage==9 && unlink((w.path+"/ui-touch-receipt-once").c_str())==0;
    uiTouchReceiptPending.store(uiReceipt);uiTouchDiagnosticsEnabled.store(uiReceipt);
    if(diagnostics){setenv("SW_FS","1",1);setenv("SW_MOVIE","1",1);}
    else{unsetenv("SW_FS");unsetenv("SW_MOVIE");}
    fprintf(stderr,"[frontend] startupDiagnostics=%d monotonic_ns=%llu externalWindowSeconds=60 externalLogLimitBytes=8388608\n",diagnostics?1:0,(unsigned long long)MonotonicNs());
    // Native-aspect output (runtime patches the game's resolution constants from XBL_SCREEN).
    // Marker files/aspect-16x9 keeps the original 16:9 output.
    {int32_t dw=0,dh=0;bool keep169=access((w.path+"/aspect-16x9").c_str(),F_OK)==0;
     if(!keep169 && OH_NativeDisplayManager_GetDefaultDisplayWidth(&dw)==DISPLAY_MANAGER_OK && OH_NativeDisplayManager_GetDefaultDisplayHeight(&dh)==DISPLAY_MANAGER_OK && dw>0 && dh>0){
        // Optional cap below 2:1 (files/aspect-cap), presenter pillarboxes the rest. Not needed since the runtime carries
        // iOS's GPU allocator budget 512 MB (the Mate 90 2848x1320 start-up null allocation was the 260 MB budget).
        {const int lw=dw>dh?dw:dh,sh=dw>dh?dh:dw;if(sh>0&&lw*100>=sh*199&&access((w.path+"/aspect-cap").c_str(),F_OK)==0){const int capped=int(sh*1.98)/8*8;if(dw>dh)dw=capped;else dh=capped;}}
        char screen[32];snprintf(screen,sizeof(screen),"%dx%d",dw,dh);
        // files/screen-override holds "WxH" (testing / wide-screen workaround): replaces the display size given to the game.
        if(FILE *so=fopen((w.path+"/screen-override").c_str(),"rb")){int ow=0,oh=0;if(fscanf(so,"%dx%d",&ow,&oh)==2&&ow>0&&oh>0)snprintf(screen,sizeof(screen),"%dx%d",ow,oh);fclose(so);}
        setenv("XBL_SCREEN",screen,1);}
     else unsetenv("XBL_SCREEN");
     fprintf(stderr,"[frontend] XBL_SCREEN=%dx%d keep169=%d\n",dw,dh,keep169?1:0);}
    gFilesDir=w.path;{const bool fast=ReadFastMode();if(fast){char v[16];snprintf(v,sizeof(v),"%.2f",kFastScale3d);setenv("XBL_SCALE3D",v,1);}else unsetenv("XBL_SCALE3D");fprintf(stderr,"[frontend] scaleMode=%s\n",fast?"fast":"quality");}
    crash_recorder::install(w.path);
    void *handle=dlopen("libxblr_execution.so",RTLD_NOW|RTLD_LOCAL);
    if(!handle){const char *error=dlerror();w.report=std::string("runtimeLoad=FAIL error=")+(error?error:"unknown");Save(w,w.report+"\n");return nullptr;}
    runtimeHandle.store(handle);
    auto focus=reinterpret_cast<int(*)(int)>(dlsym(handle,"xblr_set_focus"));if(focus)focus(foregroundState.load());
    // Keep the executable mapping alive until process exit; the bridge is one-shot.
    auto execute=reinterpret_cast<int(*)(uint32_t,uint64_t,xblr_execution_report*,size_t)>(dlsym(handle,"xblr_execute_startup"));
    if(!execute){w.report="runtimeSymbol=FAIL";Save(w,w.report+"\n");return nullptr;}
    auto configure=reinterpret_cast<int(*)(const char*,const char*,const char*)>(dlsym(handle,"xblr_configure_game_paths"));
    if(configure){
        const std::string romfs=w.path+"/game-data",saves=w.path+"/saves",temporary=w.path+"/session-temp";
        int configured=configure(romfs.c_str(),saves.c_str(),temporary.c_str());
        Save(w,"configureGamePaths="+std::to_string(configured)+"\n");
        if(configured){w.report="configureGamePaths=FAIL rc="+std::to_string(configured);Save(w,w.report);return nullptr;}
    }else{w.report="configureGamePaths=UNAVAILABLE";Save(w,w.report);return nullptr;}
    auto overlay=reinterpret_cast<int(*)(const char*)>(dlsym(handle,"xblr_configure_game_overlay"));
    // ASTC texture overlay (iOS astc_pack + casc_overlay output, one archive) when present; files/astc-off disables it.
    // touch-overlay = the full iOS overlay (touch layouts, bottom bar v4, no pad glyphs, large font, ASTC textures) and wins
    // when present; files/touch-off falls back to the texture-only astc-overlay.
    const std::string touchOverlay=w.path+"/touch-overlay";
    const bool useTouch=access((touchOverlay+"/data/.build.config").c_str(),R_OK)==0 && access((w.path+"/touch-off").c_str(),F_OK)!=0 && access((w.path+"/astc-off").c_str(),F_OK)!=0;
    const std::string astcOverlay=useTouch?touchOverlay:w.path+"/astc-overlay";
    const bool useAstc=access((astcOverlay+"/data/.build.config").c_str(),R_OK)==0 && access((w.path+"/astc-off").c_str(),F_OK)!=0;
    SetTouchHudV4(useTouch);
    int overlayRc=overlay?overlay(useAstc?astcOverlay.c_str():nullptr):-1;
    if(overlay && useAstc && overlayRc){Save(w,"configureGameOverlay=ASTC_REJECTED rc="+std::to_string(overlayRc)+"\n");overlayRc=overlay(nullptr);}
    Save(w,std::string("configureGameOverlay=")+(useAstc&&!overlayRc?(useTouch?"TOUCH":"ASTC"):"OFF")+" rc="+std::to_string(overlayRc)+"\n");
    if(overlayRc){w.report="configureGameOverlay=FAIL";Save(w,w.report);return nullptr;}
    if(w.stage==9){
        int keyboard=RegisterGameKeyboard(handle);if(keyboard){w.report="gameKeyboardRegistration=FAIL rc="+std::to_string(keyboard);Save(w,w.report);return nullptr;}
        auto resolver=reinterpret_cast<int(*)(void *(*)(const char *))>(dlsym(handle,"xblr_set_nvn_resolver"));
        auto invoke=reinterpret_cast<GuestInvoke>(dlsym(handle,"xblr_invoke_guest_callback"));
        auto fatal=reinterpret_cast<GuestFatal>(dlsym(handle,"xblr_graphics_fatal"));
        if(!resolver||!invoke||!fatal){w.report="gameRuntimeCallbacks=MISSING";Save(w,w.report);return nullptr;}
        if(!InitializeGameGraphics(w.path,invoke,fatal,w.report)){Save(w,"gameGraphics=FAIL "+w.report+"\n");return nullptr;}
#ifdef XBL_GAME_GRAPHICS
        if(diagnostics){
            const int profiling=xbg_game_profile_cpu(121,120);
            fprintf(stderr,"[frontend] graphicsCpuProfile first=121 count=120 rc=%d monotonic_ns=%llu\n",profiling,(unsigned long long)MonotonicNs());
            if(profiling!=0){w.report="graphicsCpuProfile=FAIL rc="+std::to_string(profiling);Save(w,w.report);return nullptr;}
        }
#endif
        Save(w,w.report+"\n");int result=resolver(ResolveGameGraphics);
        if(result){w.report="gameResolver=FAIL rc="+std::to_string(result);Save(w,w.report);return nullptr;}
    }
    Save(w,"runtimeLoad=PASS runtimeSymbol=PASS entering xblr_execute_startup("+std::to_string(w.stage)+","+std::to_string(w.maxGates)+")\n");
    if(diagnostics)diagnosticsStartedNs.store(MonotonicNs());
    if(w.stage==9)AttachGameTouchRuntime(handle);
    if(w.stage==9)AttachControllerRuntime(reinterpret_cast<XblInputSink>(dlsym(handle,"xblr_set_input")),reinterpret_cast<XblInputSamples>(dlsym(handle,"xblr_get_input_samples")));
    xblr_execution_report r{};int rc=execute(w.stage,w.maxGates,&r,sizeof(r));
    if(w.stage==9){StopControllers();CancelGameTouch();CancelGameKeyboard();}
    if(w.stage==9){out << "gamePresentedFrames=" << GamePresentedFrames() << '\n' << GameGraphicsStats() << '\n';}
    auto audioCalls=reinterpret_cast<uint64_t(*)()>(dlsym(handle,"xblr_audio_render_calls"));
    if(audioCalls)out << "audioRenderCallbacks=" << audioCalls() << '\n';
    out << "runtimeRc=" << rc << " abi_version=" << r.abi_version << " struct_bytes=" << r.struct_bytes << " expected_bytes=" << sizeof(r)
        << " status=" << r.status << " stage_entered=" << r.stage_entered << " stage_completed=" << r.stage_completed << " page_bytes=" << r.page_bytes << '\n';
    out << std::hex << "gates=0x" << r.gates << " max_gates=0x" << r.max_gates << " original_pc=0x" << r.original_pc << " guest_sp=0x" << r.guest_sp << " svc_number=0x" << r.svc_number << '\n';
    for(unsigned i=0;i<6;i++)out << "svc_args[" << i << "]=0x" << r.svc_args[i] << '\n';
    out << "relocations_applied=0x" << r.relocations_applied << " symbol_address=0x" << r.symbol_address << " result_x0=0x" << r.result_x0 << '\n';
    out << "symbol=" << std::string(r.symbol,strnlen(r.symbol,sizeof(r.symbol))) << '\n';
    if(strncmp(r.symbol,"nnosInitialize",sizeof(r.symbol))==0&&r.symbol_address>=0x351300){uint64_t base=r.symbol_address-0x351300;out << "sdk_base=0x" << base << " original_pc_sdk_offset=0x" << r.original_pc-base << '\n';}
    out << "sdkOSInitialization=" << ((r.stage_completed>=2&&r.abi_version==1&&r.struct_bytes==sizeof(r))?"RETURNED":"STOPPED")
        << " nnMain=" << (r.stage_entered>=9?(r.stage_completed>=9?"RETURNED":"ENTERED_THEN_STOPPED"):"NOT_EXECUTED") << '\n';
#else
    out << "runtime=NOT_CONFIGURED\n";
#endif
    w.report=out.str();Save(w,w.report);return nullptr;
}
void Execute(napi_env,void *data){auto &w=*static_cast<Work *>(data);pthread_attr_t attr;int rc=pthread_attr_init(&attr);if(rc){w.report="pthread_attr_init failed="+std::to_string(rc);return;}
    rc=pthread_attr_setstacksize(&attr,4*1024*1024);pthread_t thread{};
    if(!rc)rc=pthread_create(&thread,&attr,Thread,data);pthread_attr_destroy(&attr);
    if(rc){w.report="runtime thread create failed="+std::to_string(rc);Save(w,w.report);return;}
    rc=pthread_join(thread,nullptr);if(rc){w.report+="\npthread_join failed="+std::to_string(rc);Save(w,w.report);}
}
void Complete(napi_env env,napi_status status,void *data){std::unique_ptr<Work>w(static_cast<Work *>(data));napi_value text{};if(status!=napi_ok)w->report="runtime async completion failed";napi_create_string_utf8(env,w->report.c_str(),w->report.size(),&text);napi_resolve_deferred(env,w->deferred,text);napi_delete_async_work(env,w->work);}
}
napi_value QueueRuntime(napi_env env,napi_callback_info info,bool game){
    char path[1024]{};size_t argc=1,n=0;napi_value argv[1]{};
    if(napi_get_cb_info(env,info,&argc,argv,nullptr,nullptr)!=napi_ok||argc!=1||napi_get_value_string_utf8(env,argv[0],path,sizeof(path),&n)!=napi_ok||n==0||n>=sizeof(path)-1){napi_throw_error(env,nullptr,"Expected application files directory");return nullptr;}
    if(started.exchange(true)){napi_throw_error(env,nullptr,"Startup bridge already attempted; restart application");return nullptr;}
    auto w=std::make_unique<Work>();w->path=path;if(game){w->stage=9;w->maxGates=0;}napi_value name{},promise{};
    if(napi_create_string_utf8(env,"RuntimeStartup",NAPI_AUTO_LENGTH,&name)!=napi_ok||napi_create_promise(env,&w->deferred,&promise)!=napi_ok||napi_create_async_work(env,nullptr,name,Execute,Complete,w.get(),&w->work)!=napi_ok||napi_queue_async_work(env,w->work)!=napi_ok){if(w->work)napi_delete_async_work(env,w->work);napi_throw_error(env,nullptr,"Could not queue startup worker");return nullptr;}
    w.release();return promise;
}
napi_value RunRuntimeStartup(napi_env env,napi_callback_info info){return QueueRuntime(env,info,false);}
napi_value RunGame(napi_env env,napi_callback_info info){return QueueRuntime(env,info,true);}
bool GameDiagnosticsEnabled(){return diagnosticsEnabled.load();}
napi_value RuntimeProgress(napi_env env,napi_callback_info){
    std::string result="正在加载运行库";
#ifdef XBL_RUNTIME_EXECUTION
    void *handle=runtimeHandle.load();
    if(handle){auto get=reinterpret_cast<int(*)(xblr_execution_progress*,size_t)>(dlsym(handle,"xblr_get_execution_progress"));xblr_execution_progress p{};
        if(get&&get(&p,sizeof(p))==0)result="阶段 "+std::to_string(p.stage_entered)+"/9，已完成 "+std::to_string(p.stage_completed)+"，状态 "+std::to_string(p.status)+"，已提交游戏帧 "+std::to_string(GamePresentedFrames());}
#endif
    if(GameSurfacePaused())result+="，等待前台窗口恢复";
    static uint64_t last=0;static unsigned samples=0;uint64_t now=MonotonicNs();
#ifdef XBL_RUNTIME_EXECUTION
    static unsigned threadSamples=0;
    const uint64_t diagnosticStart=diagnosticsStartedNs.load();
    if(diagnosticStart && threadSamples<6 && now-diagnosticStart>=uint64_t(threadSamples+1)*10000000000ULL){
        ++threadSamples;
        auto snapshotFn=reinterpret_cast<int(*)(xblr_thread_snapshot*,size_t)>(dlsym(runtimeHandle.load(),"xblr_get_thread_snapshot"));
        xblr_thread_snapshot snapshot{};
        const int rc=snapshotFn?snapshotFn(&snapshot,sizeof(snapshot)):XBLR_EXEC_MISSING_SYMBOL;
        // The runtime has released its thread lock before any formatting or IO.
        std::ostringstream observation;
        observation << "[frontend-threads] pid=" << getpid() << " sample=" << threadSamples << " monotonic_ns=" << now
            << " focus=" << foregroundState.load() << " focusEpoch=" << focusEpoch.load() << " rc=" << rc;
        if(rc==0){
            observation << " snapshot_ns=" << snapshot.monotonic_ns << " count=" << snapshot.count << " truncated=" << snapshot.truncated;
            for(uint32_t i=0;i<snapshot.count && i<64;++i){const auto &t=snapshot.threads[i];
                observation << "\n[frontend-thread] sample=" << threadSamples << " id=" << t.id << " handle=" << t.handle
                    << " started=" << t.started << " exited=" << t.exited << " wait_kind=" << t.wait_kind
                    << " svc_count=" << t.svc_count << " last_svc=" << t.last_svc << std::hex << " entry=0x" << t.entry
                    << " last_lr=0x" << t.last_lr << " wait_address=0x" << t.wait_address << " wait_key=0x" << t.wait_key
                    << std::dec << " wait_sequence=" << t.wait_sequence;
            }
        }
        fprintf(stderr,"%s\n",observation.str().c_str());
    }
#endif
    if(GameDiagnosticsEnabled() && runtimeHandle.load() && samples<120 && now-last>=10000000000ULL){
        last=now;++samples;auto stats=GameGraphicsStats();
        auto audio=reinterpret_cast<uint64_t(*)()>(dlsym(runtimeHandle.load(),"xblr_audio_render_calls"));
        fprintf(stderr,"[frontend-progress] pid=%d monotonic_ns=%llu focus=%d focusEpoch=%llu frames=%llu audio=%llu %s\n",getpid(),(unsigned long long)now,foregroundState.load(),(unsigned long long)focusEpoch.load(),(unsigned long long)GamePresentedFrames(),(unsigned long long)(audio?audio():0),stats.c_str());
    }
    napi_value text{};napi_create_string_utf8(env,result.c_str(),result.size(),&text);return text;
}
napi_value SetGameFocus(napi_env env,napi_callback_info info){
    size_t n=1;napi_value arg{};int32_t state=0;
    if(napi_get_cb_info(env,info,&n,&arg,nullptr,nullptr)!=napi_ok||n!=1||napi_get_value_int32(env,arg,&state)!=napi_ok||state<1||state>3){napi_throw_error(env,nullptr,"Invalid focus state");return nullptr;}
    foregroundState.store(state);SetControllerForeground(state==1);SetGameSurfaceForeground(state==1);focusEpoch.fetch_add(1);if(GameDiagnosticsEnabled()&&runtimeHandle.load())fprintf(stderr,"[frontend-focus] pid=%d monotonic_ns=%llu state=%d epoch=%llu\n",getpid(),(unsigned long long)MonotonicNs(),state,(unsigned long long)focusEpoch.load());void *h=runtimeHandle.load();if(h){if(state!=1){auto cancel=reinterpret_cast<int(*)(uint32_t,float,float)>(dlsym(h,"xblr_ui_touch"));if(cancel)cancel(3,0,0);}auto fn=reinterpret_cast<int(*)(int)>(dlsym(h,"xblr_set_focus"));if(fn)fn(state);}
    napi_value result{};napi_get_undefined(env,&result);return result;
}
napi_value SetGameTouch(napi_env env,napi_callback_info info){
    size_t n=3;napi_value args[3]{};bool active=false;double x=0,y=0;
    if(napi_get_cb_info(env,info,&n,args,nullptr,nullptr)!=napi_ok||n!=3||napi_get_value_bool(env,args[0],&active)!=napi_ok||napi_get_value_double(env,args[1],&x)!=napi_ok||napi_get_value_double(env,args[2],&y)!=napi_ok||!(x>=0&&x<=1&&y>=0&&y<=1)){napi_throw_error(env,nullptr,"Invalid normalized touch");return nullptr;}
#ifdef XBL_RUNTIME_EXECUTION
    void *h=runtimeHandle.load();if(h){auto fn=reinterpret_cast<int(*)(const xblr_touch*,uint32_t)>(dlsym(h,"xblr_set_touches"));xblr_touch t{0,float(x),float(y)};if(fn)fn(active?&t:nullptr,active?1:0);}
#endif
    napi_value result{};napi_get_undefined(env,&result);return result;
}
napi_value GetTouchHudNapi(napi_env env,napi_callback_info){
    double v[96];const int n=GetTouchHud(v,96);napi_value arr{};napi_create_array_with_length(env,size_t(n),&arr);
    for(int i=0;i<n;i++){napi_value x{};napi_create_double(env,v[i],&x);napi_set_element(env,arr,uint32_t(i),x);}
    return arr;
}
napi_value SetTouchControlsNapi(napi_env env,napi_callback_info info){
    size_t n=1;napi_value a[1]{};bool on=true;if(napi_get_cb_info(env,info,&n,a,nullptr,nullptr)!=napi_ok||n!=1||napi_get_value_bool(env,a[0],&on)!=napi_ok){napi_throw_error(env,nullptr,"Invalid touch controls flag");return nullptr;}
    SetTouchControlsEnabled(on);napi_value r{};napi_get_undefined(env,&r);return r;
}
// Dynamic 3D scale (iOS-style thermal relief): the overlay lowers / restores the 3D render scale from the measured frame
// rate. Not persisted; the 流畅 / 画质 choice stays the ceiling.
napi_value SetScale3dNapi(napi_env env,napi_callback_info info){
    size_t n=1;napi_value a[1]{};double v=0;if(napi_get_cb_info(env,info,&n,a,nullptr,nullptr)!=napi_ok||n!=1||napi_get_value_double(env,a[0],&v)!=napi_ok){napi_throw_error(env,nullptr,"Invalid scale");return nullptr;}
#ifdef XBL_RUNTIME_EXECUTION
    void *h=runtimeHandle.load();if(h){auto fn=reinterpret_cast<void(*)(float)>(dlsym(h,"xblr_set_scale3d"));if(fn)fn(float(v));}
#endif
    napi_value r{};napi_get_undefined(env,&r);return r;
}
napi_value GetScaleMode(napi_env env,napi_callback_info){napi_value r{};napi_get_boolean(env,ReadFastMode(),&r);return r;}
napi_value SetScaleMode(napi_env env,napi_callback_info info){
    size_t n=1;napi_value a[1]{};bool fast=true;if(napi_get_cb_info(env,info,&n,a,nullptr,nullptr)!=napi_ok||n!=1||napi_get_value_bool(env,a[0],&fast)!=napi_ok){napi_throw_error(env,nullptr,"Invalid scale mode");return nullptr;}
    if(!gFilesDir.empty()){FILE *f=fopen((gFilesDir+"/scale-mode").c_str(),"wb");if(f){fputs(fast?"fast":"quality",f);fclose(f);}}
#ifdef XBL_RUNTIME_EXECUTION
    void *h=runtimeHandle.load();if(h){auto fn=reinterpret_cast<void(*)(float)>(dlsym(h,"xblr_set_scale3d"));if(fn)fn(fast?kFastScale3d:0.0f);}
#endif
    napi_value r{};napi_get_undefined(env,&r);return r;
}
napi_value GetPresentedFrames(napi_env env,napi_callback_info){
    napi_value result{};napi_create_double(env,double(GamePresentedFrames()),&result);return result;
}
napi_value SetGameButtons(napi_env env,napi_callback_info info){
    size_t n=5;napi_value args[5]{};uint32_t mask=0;int32_t axes[4]{};
    if(napi_get_cb_info(env,info,&n,args,nullptr,nullptr)!=napi_ok||(n!=1&&n!=3&&n!=5)||napi_get_value_uint32(env,args[0],&mask)!=napi_ok||mask>65535){napi_throw_error(env,nullptr,"Invalid controller buttons");return nullptr;}
    for(size_t i=1;i<n;++i){if(napi_get_value_int32(env,args[i],&axes[i-1])!=napi_ok||axes[i-1]<-32767||axes[i-1]>32767){napi_throw_error(env,nullptr,"Invalid controller axes");return nullptr;}}
    if(foregroundState.load()!=1){mask=0;memset(axes,0,sizeof(axes));}
    SetVirtualController(mask,axes);
    static unsigned published=0;if((GameDiagnosticsEnabled()||inputDiagnosticsEnabled.load())&&published++<256)fprintf(stderr,"[frontend-input-publish] pid=%d monotonic_ns=%llu mask=%u lx=%d ly=%d rx=%d ry=%d focus=%d\n",getpid(),(unsigned long long)MonotonicNs(),mask,axes[0],axes[1],axes[2],axes[3],foregroundState.load());
    napi_value result{};napi_get_undefined(env,&result);return result;
}
napi_value SetInputReceiptBudget(napi_env env,napi_callback_info info){
    size_t n=1;napi_value arg{};uint32_t limit=0;
    if(napi_get_cb_info(env,info,&n,&arg,nullptr,nullptr)!=napi_ok||n!=1||napi_get_value_uint32(env,arg,&limit)!=napi_ok||limit>256){napi_throw_error(env,nullptr,"Invalid input receipt budget");return nullptr;}
    if(!GameDiagnosticsEnabled()&&!inputDiagnosticsEnabled.load()){napi_value result{};napi_get_undefined(env,&result);return result;}
    void *h=runtimeHandle.load();auto set=h?reinterpret_cast<int(*)(uint32_t)>(dlsym(h,"xblr_set_input_receipt_budget")):nullptr;
    if(!set||set(limit)!=0){napi_throw_error(env,nullptr,"Input receipt unavailable");return nullptr;}
    fprintf(stderr,"[frontend-input-receipt] pid=%d monotonic_ns=%llu budget=%u\n",getpid(),(unsigned long long)MonotonicNs(),limit);
    napi_value result{};napi_get_undefined(env,&result);return result;
}


napi_value CancelGamePresentation(napi_env env,napi_callback_info){const bool attempted=started.load();CancelGameKeyboard(attempted);if(attempted){StopControllers();CancelGameSurfaceWait();}napi_value result{};napi_get_undefined(env,&result);return result;}


napi_value CancelGameButtons(napi_env env,napi_callback_info){CancelVirtualController();napi_value result{};napi_get_undefined(env,&result);return result;}

napi_value SetGameUiTouch(napi_env env,napi_callback_info info){
    size_t n=3;napi_value args[3]{};uint32_t phase=0;double x=0,y=0;
    if(napi_get_cb_info(env,info,&n,args,nullptr,nullptr)!=napi_ok||n!=3||napi_get_value_uint32(env,args[0],&phase)!=napi_ok||phase>3||napi_get_value_double(env,args[1],&x)!=napi_ok||napi_get_value_double(env,args[2],&y)!=napi_ok||!(x>=0&&x<=1&&y>=0&&y<=1)){napi_throw_error(env,nullptr,"Invalid UI touch");return nullptr;}
    const int rc=DispatchGameUiTouch(phase,float(x),float(y));
    napi_value result{};napi_create_int32(env,rc,&result);return result;
}

int DispatchGameUiTouch(unsigned phase,float x,float y){
    if(foregroundState.load()!=1)phase=3;
    void *h=runtimeHandle.load();auto fn=h?reinterpret_cast<int(*)(uint32_t,float,float)>(dlsym(h,"xblr_ui_touch")):nullptr;
    if(fn&&phase==0&&uiTouchReceiptPending.exchange(false)){
        auto arm=reinterpret_cast<int(*)(uint32_t)>(dlsym(h,"xblr_set_ui_touch_receipt_budget"));
        const int armed=arm?arm(1):49;
        fprintf(stderr,"[frontend-ui-touch-receipt] budget=1 rc=%d monotonic_ns=%llu\n",armed,(unsigned long long)MonotonicNs());
    }
    const int rc=fn?fn(phase,x,y):49;
    static unsigned records=0;
    if((inputDiagnosticsEnabled.load()||uiTouchDiagnosticsEnabled.load())&&records++<128)fprintf(stderr,"[frontend-ui-touch] phase=%u x=%.6f y=%.6f rc=%d monotonic_ns=%llu\n",phase,x,y,rc,(unsigned long long)MonotonicNs());
    return rc;
}
