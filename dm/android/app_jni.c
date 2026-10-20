#include "../host/dm.h"
#include "services_platform.h"
#include "services_sdk.h"
#include "services_fs.h"
#include "romfs_image.h"
#include "input_backend.h"
#include <jni.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <math.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
extern void guest_thread_init(void);
extern void crash_thread_init(void);
extern uint32_t svc_thread_main_init(void);
extern int dm_android_diagnostics_flush(void);
extern void dm_vk_set_shader_root(const char *);
extern void dm_vk_set_window(void *);
extern void dm_vk_set_display_hz(int);
extern void dm_gfx_suspend(int);
extern void dm_audio_suspend(int);
extern void dm_android_real_resource_probe(const char *);

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *native_window;
static char app_files[PATH_MAX], port_root[PATH_MAX], locale_tag[32];
static char phase[96] = "等待游戏文件";
static _Atomic int configured, started, sdk_ready, main_entered, returned;
static _Atomic int input_route_ready, input_bound;
static _Atomic uint64_t presented_frames;
static dm_android_window_state window_state = {.focus=3, .thermal_status=-1};
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static char report_file[PATH_MAX], report_temp[PATH_MAX];
static void path(char *out, size_t cap, const char *base, const char *leaf) {
    if (snprintf(out, cap, "%s/%s", base, leaf) >= (int)cap) die("Android private path too long");
}
/* A fixed scale is an explicit diagnostic experiment. An absent marker keeps
 * the original 1.0 scale; reject truncated values, NaN/Inf, and extra tokens. */
static int configure_render_scale(void) {
    char marker[PATH_MAX], text[64], value[32];
    path(marker,sizeof marker,port_root,"diagnostics/render-scale.txt");
    unsetenv("DM_3D_SCALE");
    FILE *f=fopen(marker,"r");
    if (!f) return errno==ENOENT?0:errno;
    size_t n=fread(text,1,sizeof text-1,f);
    int error=ferror(f)?EIO:0;
    int trailing=fgetc(f);
    if (ferror(f)) error=EIO;
    if (fclose(f) && !error) error=errno?errno:EIO;
    if (error) return error;
    if (trailing!=EOF || memchr(text,'\0',n)) return EINVAL;
    text[n]='\0';
    char *start=text, *end;
    while (isspace((unsigned char)*start)) start++;
    errno=0; float scale=strtof(start,&end);
    if (end==start || errno==ERANGE || !isfinite(scale) || scale<0.70f || scale>1.0f) return EINVAL;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return EINVAL;
    snprintf(value,sizeof value,"%.9g",(double)scale);
    return setenv("DM_3D_SCALE",value,1)?errno:0;
}
static void status_text(char *text,size_t capacity) {
    snprintf(text,capacity,"{\"phase\":\"%s\",\"sdk_ready\":%s,\"nnMain_entered\":%s,\"presented_frames\":%llu,\"returned\":%s}",phase,
        atomic_load(&sdk_ready)?"true":"false",atomic_load(&main_entered)?"true":"false",
        (unsigned long long)atomic_load(&presented_frames),atomic_load(&returned)?"true":"false");
}
static void persist_status(void) {
    if (!report_file[0]) return;
    pthread_mutex_lock(&report_lock);
    char text[384];pthread_mutex_lock(&state_lock);status_text(text,sizeof text);pthread_mutex_unlock(&state_lock);
    int fd=open(report_temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0660),error=0;
    if(fd<0)error=errno;
    else {size_t n=strlen(text),sent=0;while(sent<n){ssize_t k=write(fd,text+sent,n-sent);if(k<0&&errno==EINTR)continue;if(k<=0){error=errno?errno:EIO;break;}sent+=(size_t)k;}
        if(!error&&fsync(fd))error=errno;if(close(fd)&&!error)error=errno;
        if(!error&&rename(report_temp,report_file))error=errno;}
    if(error)LOG("ANDROID_STATUS_WRITE_FAILED errno=%d",error);
    pthread_mutex_unlock(&report_lock);
}
static void phase_set(const char *text) {
    pthread_mutex_lock(&state_lock); snprintf(phase, sizeof phase, "%s", text); pthread_mutex_unlock(&state_lock);
    LOG("ANDROID_GAME_STAGE %s", text);
    persist_status();
}
void *dm_android_native_window(void) {
    pthread_mutex_lock(&state_lock); ANativeWindow *w = native_window; pthread_mutex_unlock(&state_lock);
    return w; /* Root owns an acquired reference until graphics are suspended/drained. */
}
void dm_android_front_presented(uint64_t frame) {
    atomic_store(&presented_frames, frame);
    if (frame == 1) phase_set("游戏画面已呈现");
}
static int suspend_graphics(int on) { dm_gfx_suspend(on); return 0; }
static int pause_audio(int on) {dm_audio_suspend(on);return 0;}
static int input_source_ready(void *opaque) {(void)opaque;return atomic_load(&input_route_ready)&&dm_android_native_window()!=NULL;}
static void *required(const char *name) {
    void *fn = guest_sym(name); if (!fn) die("Missing original startup export %s", name); return fn;
}
static void call_original(const char *name) {
    LOG("ANDROID_ORIGINAL_CALL %s", name); ((void (*)(void))required(name))();
    LOG("ANDROID_ORIGINAL_RETURN %s", name);
}
static void *game_thread(void *unused) {
    (void)unused;
    crash_thread_init();
    char debug_hold[PATH_MAX];path(debug_hold,sizeof debug_hold,port_root,"diagnostics/debug-hold");
    if(!access(debug_hold,F_OK)) { LOG("ANDROID_DEBUG_HOLD before original startup");raise(SIGSTOP); }
    char modules[PATH_MAX], rom[PATH_MAX], image[PATH_MAX], save[PATH_MAX], temp[PATH_MAX], profile[PATH_MAX];
    path(modules,sizeof modules,port_root,"modules"); path(rom,sizeof rom,port_root,"romfs-view");
    path(image,sizeof image,app_files,"game.romfs"); path(save,sizeof save,port_root,"save");
    path(temp,sizeof temp,port_root,"temp"); path(profile,sizeof profile,port_root,"profile");
    struct stat st;
    if (stat(image,&st) || st.st_size != 8973693207LL) die("Existing original RomFS image absent or wrong size");
    phase_set("正在读取游戏资源");
    int error = romfs_image_setup_checked(image,rom,8973693207ULL);
    if (error) die("RomFS image setup errno=%d",errno);
    if (dm_android_fs_ensure_directory(save) || dm_android_fs_ensure_directory(temp) || dm_android_fs_ensure_directory(profile)) die("Android private directories");
    setenv("DM_ROMFS",rom,1); setenv("DM_SAVE",save,1); setenv("DM_TEMP",temp,1);
    /* 自定义设置的目录现在就给（同 iOS 壳 main.m / Mac app_mac.m 那一句）：语言开关是在「挂存档那一刻」由 hle_fs.c 的 settings_lang_apply
       读它、再把游戏的 Settings.json 里 Language / VoiceLanguage 改成中文或英文的；那一刻存档还没挂上，dm_save_commitdir() 还是 NULL，
       dm_custom 找不到自己的文件就答 -1，于是开关永远不生效（真机症状：自定义页选了中文，重启仍是英文；日志里「自定义设置：读盘」那一行
       比「fs 挂载 save」晚几百行）。这里给 port_root，sw_custom.txt 就在它下面，与原来 save/../sw_custom.txt 是同一个文件。
       Give the Custom-settings directory right now (same line as the iOS shell main.m / Mac app_mac.m): the language switch is read by
       settings_lang_apply in hle_fs.c at the moment the save is mounted, to rewrite Language / VoiceLanguage in the game's Settings.json.
       At that moment the save is not mounted yet and dm_save_commitdir() is still NULL, so dm_custom cannot find its file, answers -1, and the
       switch never takes effect (real-device symptom: Chinese chosen on the Custom page, still English after a restart; the 「自定义设置：读盘」
       line lands hundreds of lines after 「fs 挂载 save」). Give port_root here: sw_custom.txt lives directly under it and is the same file as
       the former save/../sw_custom.txt. */
    { void dm_custom_init(const char *); dm_custom_init(port_root); }
    if (locale_tag[0]) setenv("DM_LOCALE_TAG",locale_tag,1);   /* 系统语言：首次建设置文件时写原版 Language（i18n_android.c 的 dm_sys_lang） */
    { char overlay[PATH_MAX]; path(overlay,sizeof overlay,port_root,"diagnostics/overlay");   /* 覆盖层根 = diagnostics/overlay（adb 可写通道；放进去的文件先于镜像生效） */
      setenv("DM_ROMFS_OVERLAY",overlay,1); }
    char private_vulkan[PATH_MAX];path(private_vulkan,sizeof private_vulkan,port_root,"diagnostics/private-vulkan");
    if(access(private_vulkan,F_OK)==0)setenv("DM_PRIVATE_VULKAN","turnip",1);
    dm_android_platform_config config = {.mode=DM_ANDROID_HOST_ACTIVITY,
        .profile_directory=profile, .locale=locale_tag, .locale_source="Activity.Configuration",
        /* Selected add-on set is explicitly empty. No unverified DLC is exposed. */
        .content_manifest_verified=1, .graphics_present=1, .audio_present=1,
        .suspend_and_drain_graphics=suspend_graphics, .pause_audio=pause_audio, .flush_diagnostics=dm_android_diagnostics_flush};
    if ((error=dm_android_platform_bind(&config)) || (error=dm_android_platform_register_services())) die("Android platform bind/register error=%d",error);
    { void dm_android_ui_state_install(void); dm_android_ui_state_install(); }   /* P1：UI 消息门 → 状态机（前端/面板模式） | P1: UI message gate -> state machine */
    { void dm_android_hud_install(void); dm_android_hud_install(); }
    { void dm_android_diag_install(void); dm_android_diag_install(); }   /* P8：秘境拦原版难度提示（事件门 15）+ 请游戏存盘等 */             /* P3：服务端拍门 → HUD 抬数（dm_hud） */
    pthread_mutex_lock(&state_lock); dm_android_window_state w=window_state; pthread_mutex_unlock(&state_lock);
    if ((error=dm_android_platform_update_window(&w))) die("Android initial window state error=%d",error);
    dm_android_service_register(DM_ANDROID_SERVICE_FS,dm_android_fs_initialize,NULL);
    phase_set("正在装载游戏");
    const char *names[]={"main","sdk","subsdk0"}; load_modules(modules,names,3); guest_thread_init();
    { void dm_padmode_boot(void); dm_padmode_boot(); }   /* P4：开机改布局前缀为触屏版（覆盖层有 ctrl_touch 标记时）+ 载入字符串旁路表 */
    { void target_rank_install(void); target_rank_install(); }   /* 触屏点按自动选怪：Boss > 精英 > 普通（hle_target.c）。iOS 入口 main.c 有这一句，Android 入口原先没有 */
    /* 转向回跳平滑 + 模型转身参数（dm_turnsmooth.c，观测门 19 / 20）。iOS 在 diag.c 里装，Android 原先根本没编这个文件：
       触屏摇杆走的是「手柄」那一组转向参数，掉头时原版会播一段原地转身、逻辑位置同时往回走，观感就是「转身时突然变慢甚至停下」。
       只动本地玩家、纯画面层，不改逻辑位置 / 发包 / 存档。
       Turn snap-back smoothing + model turning parameters (dm_turnsmooth.c, observation gates 19 / 20). iOS installs it in diag.c; Android never
       compiled the file at all. The touch stick goes through the "controller" turning group, so a U-turn plays the original's turn-in-place
       animation while the logical position already moves back -- felt as "turns suddenly slow down or stop". Local player only, purely visual:
       it never touches the logical position, packets or saves. */
    { void dm_turnsmooth_install(uint8_t *); module_t *tm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) tm = &g_mod[i];
      if (tm) dm_turnsmooth_install((uint8_t *)tm->base); else LOG("【红】转向平滑：找不到 main 模块，没装（掉头会走原版原地转身）"); }

    { void dm_touch_game_live_check(int); int dm_touch_game_attach(void);       /* P7：技能绑定桥（OBS_BINDING_GETTER 门 + HUD 身份核对） */
    dm_touch_game_live_check(1);
    if (!dm_touch_game_attach()) LOG("【红】技能绑定读取没挂上（产物没有观测门 OBS_BINDING_GETTER，或 main 模块不符）：技能键图标 / 回城键绑定不工作"); }

    uint32_t handle=svc_thread_main_init();
    phase_set("正在初始化游戏"); call_original("_init_libc0");
    LOG("ANDROID_ORIGINAL_CALL nnosInitialize handle=%u",handle);
    ((void (*)(uint64_t,uint64_t))required("nnosInitialize"))(handle,0);
    LOG("ANDROID_ORIGINAL_RETURN nnosInitialize");
    call_original("_init_libc1"); call_original("nninitInitializeSdkModule");
    call_original("nninitStartup"); call_original("_init_libc2"); atomic_store(&sdk_ready,1);
    dm_android_real_resource_probe(port_root);
    phase_set("正在启动游戏"); run_module_inits();
    char *args[]={"D2RMobile",NULL};
    ((void (*)(int))required("_ZN2nn2os11SetHostArgcEi"))(1);
    ((void (*)(char **))required("_ZN2nn2os11SetHostArgvEPPc"))(args);
    atomic_store(&main_entered,1);persist_status();call_original("nnMain");
    atomic_store(&returned,1); phase_set("游戏已退出"); return NULL;
}
static int copy_jstring(JNIEnv *env,jstring text,char *out,size_t cap) {
    if (!text) return EINVAL;
    const char *s=(*env)->GetStringUTFChars(env,text,NULL); if (!s) return ENOMEM;
    int rc=strlen(s)>=cap?ENAMETOOLONG:0; if (!rc) strcpy(out,s);
    (*env)->ReleaseStringUTFChars(env,text,s); return rc;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeConfigure(JNIEnv *env,jclass cls,jstring files,jstring locale,jboolean touch_route) {
    (void)cls;
    if (atomic_exchange(&configured,1)) return EALREADY;
    int rc=copy_jstring(env,files,app_files,sizeof app_files);
    if (!rc) rc=copy_jstring(env,locale,locale_tag,sizeof locale_tag);
    if (rc || app_files[0]!='/') return rc?rc:EINVAL;
    if (!touch_route) return ENODEV;
    atomic_store(&input_route_ready,1);
    path(port_root,sizeof port_root,app_files,"d2r-mobile");
    if ((rc=configure_render_scale())) { LOG("ANDROID_3D_SCALE_CONFIG_FAILED errno=%d expected finite 0.70..1.0",rc); return rc; }
    char driver_option[PATH_MAX];path(driver_option,sizeof driver_option,port_root,"diagnostics/private-vulkan");
    if(access(driver_option,F_OK)==0)setenv("DM_PRIVATE_VULKAN","turnip",1);
    /* 诊断通道（同上面那几个标记文件）：放一个空文件 diagnostics/target-log 就把自动选怪的候选记录打开。
       Android 不透传任意环境变量，而这个开关要靠真机读数才验得了（宿主注册成功只说明门挂上了，看不到分数有没有被抬高）。
       touch_log 用的是 <port_root> 下的路径，写在那儿 adb 就能 pull 走。
       Diagnostic channel (same marker-file pattern as above): dropping an empty diagnostics/target-log file turns on the auto-targeting candidate log.
       Android does not pass arbitrary environment variables through, and this switch can only be verified by readings on a real device (a successful
       host registration only shows the gate is attached, not that scores are being raised). touch_log writes under <port_root>, where adb can pull it. */
    path(driver_option,sizeof driver_option,port_root,"diagnostics/target-log");
    if(access(driver_option,F_OK)==0){ path(driver_option,sizeof driver_option,port_root,"diagnostics/target-log.tsv");
        setenv("DM_TARGET_LOG",driver_option,1); LOG("ANDROID_TARGET_LOG %s",driver_option); }
    path(driver_option,sizeof driver_option,port_root,"diagnostics/private-vulkan-sysmem");
    if(access(driver_option,F_OK)==0){setenv("DM_PRIVATE_VULKAN","turnip",1);setenv("TU_DEBUG","sysmem",1);}
    path(driver_option,sizeof driver_option,port_root,"diagnostics/vulkan-validation");
    if(access(driver_option,F_OK)==0)setenv("DM_VULKAN_VALIDATION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-pass-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PASS_REUSE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-cpu-uniform-snapshot");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_CPU_UNIFORM_SNAPSHOT","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-pso-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PSO_REUSE","1",1);
    unsetenv("DM_PSO_LAST_CACHE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-pso-last-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_PSO_LAST_CACHE","1",1);
    /* 翻译队列 + 25ms 出图节拍（2026-10-19 实测，营地标准档）：
         同步模式（DM_NVN_SYNC=1）下 replay 直接跑在游戏线程里，游戏线程每帧 25.2ms 花在
         nvnQueueSubmitCommands 内部当场执行 replay；开启队列后同一项降到 0.020ms——那不是
         变快，是把这份 CPU 工作搬到另一根核上。总 CPU 反而升高（197% → 223%），但帧时间从
         "逻辑 + replay 相加"变成两核取 max，且 GPU 不再等喂料（util 84% → 93%）。
         但只开队列不够——默认节拍是 33.3ms（游戏 interval 2 @60Hz），本身就封顶 30fps。
         两个一起开才是收益点：交替两轮 A/B 中位 36.11/35.90 fps vs 同步 30.50/30.12（+18~19%），
         GPU util 97–100%（此时是 GPU 打满，CPU 有余量）。
         用户提供的事实：游戏逻辑固定 25 拍，出图节拍只是呈现节奏，所以 25ms 节拍不改逻辑速度。
         回退：disable-nvn-queue 回同步；env.txt 的 DM_PRESENT_HZ 覆盖节拍。
       Translation queue + 25 ms present cadence (measured 2026-10-19, camp standard tier):
       in sync mode replay runs on the game thread and the whole 25.2 ms/frame is spent waiting
       inside nvnQueueSubmitCommands; with the queue it drops to 0.020 ms. The queue alone is not
       enough because the default cadence is 33.3 ms (game interval 2 @60 Hz), which caps at 30 fps.
       Alternating A/B medians: 36.11/35.90 fps vs 30.50/30.12 (+18-19%), GPU utilisation 97-100%.
       The game logic is a fixed 25 ticks; the cadence is only presentation, so this does not change
       logic speed. Rollback: diagnostics/disable-nvn-queue restores sync; DM_PRESENT_HZ overrides. */
    unsetenv("DM_NVN_SYNC");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-nvn-queue");
    if(access(driver_option,F_OK)==0)setenv("DM_NVN_SYNC","1",1);
    if(!getenv("DM_PRESENT_HZ"))setenv("DM_PRESENT_HZ","80",1);
    /* 附件 store 跳过（深度通道）：同 fps 下把附件流量从 891.8 降到 691.8 MB/帧（−22%），
       给复杂场景留 GPU 余量。营地/营地外静态对拍无可检测差异（分区热力图与噪声同形态），
       但战斗/特效/过场未验证，且 Vulkan 规范允许 DONT_CARE 内容为任意值——所以**默认关**，
       仅在 diagnostics/enable-att-skip 存在时打开。删掉该文件即回退。
       Attachment store elision for depth passes: same fps, attachment traffic 891.8 -> 691.8 MB/frame
       (-22%), leaving GPU headroom for heavy scenes. No detectable difference in static camp/field
       shots (the per-cell diff map matches the noise shape), but combat/effects/cutscenes are not
       validated and Vulkan allows DONT_CARE to leave arbitrary contents -- so off by default,
       enabled only when diagnostics/enable-att-skip exists. Delete the file to roll back. */
    /* 附件冗余带宽削减现在**默认开**（哨兵探针已证明颜色附件全覆盖；深度 store 跳过在
       营地/野外静态对拍无可检测差异）。两个回退文件见下，删/建即切。
       Attachment-bandwidth elision now defaults to ON; see the rollback files below. */
    setenv("DM_ATT_STORE_SKIP","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-att-skip");
    if(access(driver_option,F_OK)==0)setenv("DM_ATT_STORE_SKIP","0",1);
    /* loadOp=DONT_CARE 默认关：哨兵探针实测某个 2270x1056 通道只覆盖 8%，换 DONT_CARE 会露未定义内容。
       要试就写 env.txt 的 DM_ATT_LOAD_SKIP=1。 */
    setenv("DM_ATT_LOAD_SKIP","0",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-att-load-skip");
    if(access(driver_option,F_OK)==0)setenv("DM_ATT_LOAD_SKIP","0",1);
    setenv("DM_WRITEBACK_PREPARE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-writeback-prepare");
    if(access(driver_option,F_OK)==0)setenv("DM_WRITEBACK_PREPARE","0",1);
    /* Android 实测：遮挡查询的批量重置路径在 Adreno 上每帧多烧 ~11ms（营地 +32% fps），
       默认关批量；文件 enable-query-batch 可恢复他原版行为做 A/B。
       Measured on Adreno: the batched occlusion-query reset path burns ~11ms/frame
       (camp scene +32% fps). Batching is off by default; enable-query-batch restores
       the original behaviour for A/B. */
    setenv("DM_DISABLE_QUERY_BATCH","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-query-batch");
    if(access(driver_option,F_OK)==0)unsetenv("DM_DISABLE_QUERY_BATCH");
    unsetenv("DM_WRITEBACK_COALESCE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/writeback-coalesce");
    if(access(driver_option,F_OK)==0)setenv("DM_WRITEBACK_COALESCE","1",1);
    unsetenv("DM_COMMAND_STATE_CACHE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/command-state-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_COMMAND_STATE_CACHE","1",1);
    setenv("DM_DIRTY_SUMMARY","1",1);unsetenv("DM_DIRTY_QUERY_STATS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-dirty-summary");
    if(access(driver_option,F_OK)==0)setenv("DM_DIRTY_SUMMARY","0",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/dirty-query-stats");
    if(access(driver_option,F_OK)==0)setenv("DM_DIRTY_QUERY_STATS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-driver-table-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DRIVER_TABLE_REUSE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-temp-pool");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_TEMP_POOL","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-storage-bias");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_STORAGE_BIAS","1",1);
    unsetenv("DM_DIAG_IMG_USAGE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-img-usage-1");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_IMG_USAGE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-img-usage-2");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_IMG_USAGE","2",1);
    unsetenv("DM_BARRIER_PASS_ELIDE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/barrier-pass-elide");
    if(access(driver_option,F_OK)==0)setenv("DM_BARRIER_PASS_ELIDE","1",1);
    unsetenv("DM_DIAG_NO_PASS_DEPS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-no-pass-deps");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_NO_PASS_DEPS","1",1);
    unsetenv("DM_DISABLE_PRECISE_SYNC");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-precise-sync");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PRECISE_SYNC","1",1);
    unsetenv("DM_HZ_DIAG");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/hz-diag.txt");
    {FILE *hf=fopen(driver_option,"r");if(hf){char hv[8]={0};size_t hn=fread(hv,1,7,hf);fclose(hf);if(hn)setenv("DM_HZ_DIAG",hv,1);}}
    unsetenv("DM_CHUNK_DRAWS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/chunk-draws.txt");
    {FILE *cf=fopen(driver_option,"r");if(cf){char cv[12]={0};size_t cn=fread(cv,1,11,cf);fclose(cf);if(cn)setenv("DM_CHUNK_DRAWS",cv,1);}}
    unsetenv("DM_PROFILE_DETAIL");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/profile-detail");
    if(access(driver_option,F_OK)==0)setenv("DM_PROFILE_DETAIL","1",1);
    unsetenv("DM_DIAG_SKIP_DRAWS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-skip-draws");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_SKIP_DRAWS","1",1);
    unsetenv("DM_DIAG_SKIP_XFER");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-xfer.txt");
    {FILE *xf=fopen(driver_option,"r");if(xf){char xv[8]={0};size_t xn=fread(xv,1,7,xf);fclose(xf);if(xn)setenv("DM_DIAG_SKIP_XFER",xv,1);}}
    unsetenv("DM_DISABLE_SUBRES");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-subres");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SUBRES","1",1);
    unsetenv("DM_DISABLE_SETRT_SKIP");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-setrt-skip");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SETRT_SKIP","1",1);
    unsetenv("DM_DISABLE_WB_STAGE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-wb-stage");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_WB_STAGE","1",1);
    unsetenv("DM_DIAG_SKIP_CLASS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-class.txt");
    {FILE *kf=fopen(driver_option,"r");if(kf){char kv[8]={0};size_t kn=fread(kv,1,7,kf);fclose(kf);if(kn)setenv("DM_DIAG_SKIP_CLASS",kv,1);}}
    unsetenv("DM_DIAG_SHAPE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-shape.txt");
    {FILE *hf2=fopen(driver_option,"r");if(hf2){char hv2[8]={0};size_t hn2=fread(hv2,1,7,hf2);fclose(hf2);if(hn2)setenv("DM_DIAG_SHAPE",hv2,1);}}
    unsetenv("DM_NARROW_ACCESS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/narrow-access");
    if(access(driver_option,F_OK)==0)setenv("DM_NARROW_ACCESS","1",1);
    unsetenv("DM_DISABLE_WAIT_ELISION");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-wait-elision");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_WAIT_ELISION","1",1);
    unsetenv("DM_DISABLE_ASYNC_COMMIT");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-async-commit");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_ASYNC_COMMIT","1",1);
    unsetenv("DM_CONFLICT_WAIT_ALL");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/conflict-wait-all");
    if(access(driver_option,F_OK)==0)setenv("DM_CONFLICT_WAIT_ALL","1",1);
    unsetenv("DM_DIAG_FIXED_DESC");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-fixed-desc");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_FIXED_DESC","1",1);
    unsetenv("DM_DIAG_NO_OCCLUSION");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-no-occlusion");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_NO_OCCLUSION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-query-batch");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_QUERY_BATCH","1",1);
    setenv("DM_QUERY_RESET_RANGE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-query-reset-range");
    if(access(driver_option,F_OK)==0)setenv("DM_QUERY_RESET_RANGE","0",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-desc-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DESC_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-resource-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_RESOURCE_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-dirty-uploads");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DIRTY_UPLOADS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-control-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_CONTROL_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-loadstore-store");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_LOADSTORE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-loadstore-both");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_LOADSTORE","2",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-barrier-elision");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_BARRIER_ELISION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-sample-copy-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SAMPLE_COPY_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-ps.txt");
    {FILE *sf=fopen(driver_option,"r");if(sf){char sk[17]={0};size_t n=fread(sk,1,16,sf);fclose(sf);if(n==16)setenv("DM_SKIP_PS",sk,1);}}
    unsetenv("DM_EVENT_TRACE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/event-trace");
    if(access(driver_option,F_OK)==0)setenv("DM_EVENT_TRACE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/pass-timing");
    if(access(driver_option,F_OK)==0)setenv("DM_PASS_TIMING","1",1);
    /* Hint sessions are an explicit A/B experiment, off when the marker is absent. */
    unsetenv("DM_PERFORMANCE_HINTS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/performance-hints");
    if(access(driver_option,F_OK)==0)setenv("DM_PERFORMANCE_HINTS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/capture-frames");
    if(access(driver_option,F_OK)==0)setenv("DM_CAPTURE_FRAMES","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-barrier-cmd");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_BARRIER","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-barrier-soft");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_BARRIER","2",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/barrier-stats");
    if(access(driver_option,F_OK)==0)setenv("DM_BARRIER_STATS","1",1);
    char logs[PATH_MAX], log[PATH_MAX], shaders[PATH_MAX];
    path(logs,sizeof logs,port_root,"diagnostics");
    setenv("DM_FRAME_DIR",logs,1);
    if (dm_android_fs_ensure_directory(logs)) return errno?errno:EIO;
    /* TEST PATCH (not in dodola's tree): switch files -> setenv, using the same access()
       channel as dodola's own flag files (the app-side sandbox allows stat on shell-created
       files but not always read of their contents, so one file per switch/value). */
    { static const struct { const char *file, *name, *value; } env_flags[] = {
        {"flag-precise-sync-off",  "DM_DISABLE_PRECISE_SYNC",          "1"},
        {"flag-binding-plans-off", "DM_DISABLE_BINDING_PLANS",         "1"},
        {"flag-snapshot-off",      "DM_DISABLE_CPU_UNIFORM_SNAPSHOT",  "1"},
        {"flag-storage-bias-off",  "DM_DISABLE_STORAGE_BIAS",          "1"},
        {"flag-occlusion-off",     "DM_DIAG_NO_OCCLUSION",             "1"},
        {"flag-temp-pool-off",     "DM_DISABLE_TEMP_POOL",             "1"},
        {"flag-res-cache-off",     "DM_DISABLE_RESOURCE_CACHE",        "1"},
        {"flag-pso-reuse-off",     "DM_DISABLE_PSO_REUSE",             "1"},
        {"flag-wb-stage-off",      "DM_DISABLE_WB_STAGE",              "1"},
        {"flag-async-drain",       "DM_ASYNC_DRAIN",                   "1"},
        {"flag-scale-70",          "DM_3D_SCALE",                      "0.70"},
        {"flag-scale-75",          "DM_3D_SCALE",                      "0.75"},
        {"flag-scale-80",          "DM_3D_SCALE",                      "0.80"},
        {"flag-scale-85",          "DM_3D_SCALE",                      "0.85"},
        {"flag-3d-ref-native",     "DM_3D_SCALE_REF",                  "2168x1220"},
        {"flag-3d-ref-2624",       "DM_3D_SCALE_REF",                  "2624x1220"},
        {"flag-bright-k1",         "DM_BRIGHT_K",                      "1"},
        {"flag-bright-k25",        "DM_BRIGHT_K",                      "2.5"},
        {"flag-3d-scale-dyn",      "DM_3D_SCALE_DYN",                  "1"},
        {"flag-3d-floor-85",       "DM_3D_SCALE_FLOOR",                "0.85"},
        {"flag-3d-floor-80",       "DM_3D_SCALE_FLOOR",                "0.80"},
        {"flag-3d-floor-75",       "DM_3D_SCALE_FLOOR",                "0.75"},
        {"flag-chunk-12",          "DM_CHUNK_DRAWS",                   "12"},
        {"flag-chunk-24",          "DM_CHUNK_DRAWS",                   "24"},
        {"flag-chunk-96",          "DM_CHUNK_DRAWS",                   "96"},
        {"flag-chunk-192",         "DM_CHUNK_DRAWS",                   "192"},
        {"flag-chunk-384",         "DM_CHUNK_DRAWS",                   "384"},
        {"flag-pass-timing",       "DM_PASS_TIMING",                   "1"},
        {"flag-loadstore-1",       "DM_DIAG_LOADSTORE",                 "1"},
        {"flag-loadstore-2",       "DM_DIAG_LOADSTORE",                 "2"},
        {"flag-loadstore-3",       "DM_DIAG_LOADSTORE",                 "3"},
        {"flag-gameres-1080",      "DM_GAME_RES",                       "1920x1080"},
        {"flag-gameres-native",    "DM_GAME_RES",                       "2168x1220"},
        {"flag-gameres-2624",      "DM_GAME_RES",                       "2624x1220"},
        {"flag-gameres-switch",    "DM_GAME_RES",                       "1600x900"},
        {"flag-scale-100",         "DM_3D_SCALE",                       "1.0"},
        {"flag-present-hz-120",    "DM_PRESENT_HZ",                     "120"},
        {"flag-present-hz-30",     "DM_PRESENT_HZ",                     "30"},
        {"flag-resource-stats",    "DM_RESOURCE_STATS",                 "1"},
      };
      /* 画质档（diagnostics/quality.txt，一行档名）：一次写好出图分辨率 + 3D 比例两个值。
         优先级：quality.txt < flag-* < env.txt（后两者都能覆盖单值）。2026-10-16 固化缺省档
         = 2624-075（真窗口出图 1:1 + 世界 0.75 = 1968×915，docs/android-perf-notes.md §7）。
         Quality tiers (diagnostics/quality.txt, one tier name per line): sets both the output
         resolution and the 3D scale at once. Precedence: quality.txt < flag-* < env.txt.
         Default since 2026-10-16 = 2624-075 (true-window 1:1 output + 0.75 world = 1968x915). */
      { char qf[PATH_MAX]; path(qf, sizeof qf, logs, "quality.txt");
        FILE *q = fopen(qf, "r");
        if (q) {
          char line[64] = "", tok[32] = "";
          if (fgets(line, sizeof line, q)) sscanf(line, "%31s", tok);
          fclose(q);
          /* 档位改成"预算档"，不再写死分辨率：low / standard / high 只把档号交给 DM_QUALITY_TIER，
             出图与世界像素由屏幕尺寸 × 预算派生（dm/android/diagnostics.c 的 front_game_res、
             dm/host/hle_oe.c 的 3D 比例）。老档名（2624-075 等）已删除，写进去会打红日志并回落到
             界面档位；1600-100 这类对照档才继续写 DM_GAME_RES / DM_3D_SCALE 的原值（调试用）；native 用档位+显式 1.0 表达。
             Tiers are now budget tiers and no longer pin a resolution: low / standard / high only pass the tier
             number through DM_QUALITY_TIER, and the output and world pixels are derived from the screen size x
             budgets (front_game_res in dm/android/diagnostics.c, the 3D scale in dm/host/hle_oe.c). The old tier
             names (2624-075 and so on) are gone: writing one logs a failure and falls back to the UI tier;
             the comparison/debug tier 1600-100 still sets the raw DM_GAME_RES / DM_3D_SCALE values; native is expressed as tier + an explicit 1.0. */
          struct { const char *name; int tier; const char *gamres, *scale; } tiers[] = {
            {"low",        0, NULL, NULL},
            {"standard",   1, NULL, NULL},
            {"high",       2, NULL, NULL},
            {"switch",    -1, "1600x900",  "1.0"},
            {"1600-100",  -1, "1600x900",  "1.0"},
            {"native",     2, NULL,        "1.0"},
          };
          int hit = 0;
          for (unsigned i = 0; i < sizeof tiers / sizeof *tiers; i++)
            if (!strcmp(tok, tiers[i].name)) {
              if (tiers[i].tier >= 0) { char b[8]; snprintf(b, sizeof b, "%d", tiers[i].tier); setenv("DM_QUALITY_TIER", b, 1);
                  LOG("ANDROID_QUALITY tier=%s → DM_QUALITY_TIER=%d（预算档：出图/世界按屏幕派生）", tok, tiers[i].tier); }
              if (tiers[i].gamres) setenv("DM_GAME_RES", tiers[i].gamres, 1);
              if (tiers[i].scale) setenv("DM_3D_SCALE", tiers[i].scale, 1);
              if (tiers[i].gamres || tiers[i].scale) LOG("ANDROID_QUALITY tier=%s → 显式覆盖 DM_GAME_RES=%s DM_3D_SCALE=%s（调试档）", tok, tiers[i].gamres ? tiers[i].gamres : "（按预算）", tiers[i].scale ? tiers[i].scale : "（按预算）");
              hit = 1; break;
            }
          if (!hit && tok[0]) LOG("【红】ANDROID_QUALITY 未知档名「%s」（现在只有 low / standard / high；调试档 switch / 1600-100 / native。老档名 2624-075 之类已删除，忽略）", tok);
        } }
      for (unsigned i = 0; i < sizeof env_flags / sizeof *env_flags; i++) {
        char f[PATH_MAX]; path(f, sizeof f, logs, env_flags[i].file);
        if (access(f, F_OK) == 0) { setenv(env_flags[i].name, env_flags[i].value, 1); LOG("ANDROID_FLAG %s → %s=%s", env_flags[i].file, env_flags[i].name, env_flags[i].value); }
      }
    }
    /* TEST PATCH (not in dodola's tree): K=V lines in diagnostics/env.txt are imported into the
       environment, so performance bisecting can flip their DM_* switches without rebuilding. */
    { char envf[PATH_MAX]; path(envf,sizeof envf,logs,"env.txt");
      FILE *ef=fopen(envf,"r");
      if(ef){ char line[512]; while(fgets(line,sizeof line,ef)){ char *eq=strchr(line,'='); if(!eq)continue; *eq=0;
        char *k=line; char *v=eq+1; size_t n=strlen(v); while(n&&(v[n-1]=='\n'||v[n-1]=='\r'))v[--n]=0;
        if(*k)setenv(k,v,1); } fclose(ef); LOG("ANDROID_ENV_FILE imported %s",envf); } }
    path(log,sizeof log,logs,"game-startup.log");
    char status_path[PATH_MAX]; path(status_path,sizeof status_path,logs,"status.json");
    if (unlink(status_path) && errno!=ENOENT) return errno;
    strcpy(report_file,status_path);path(report_temp,sizeof report_temp,logs,"status.native.new");
    int fd=open(log,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0660); if (fd<0) return errno;
    if (dup2(fd,STDERR_FILENO)<0) {rc=errno;close(fd);return rc;} close(fd); setvbuf(stderr,NULL,_IONBF,0);
    g_t0_ns=now_ns(); g_flag.fs=1; g_flag.svc=1; g_flag.dbg=1;
    path(shaders,sizeof shaders,port_root,"shaders"); dm_vk_set_shader_root(shaders);
    LOG("ANDROID_HOST app_uid=%u min_api=29 original_modules=3 selected_addons=0 no_CPU_interpreter",getuid());
    return 0;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeSurface(JNIEnv *env,jclass cls,jobject surface,jint width,jint height,jfloat density,jfloat hz,jint focus) {
    (void)cls;
    ANativeWindow *next=surface?ANativeWindow_fromSurface(env,surface):NULL;
    if (surface && !next) return EINVAL;
    /* Suspend before replacement; no guest can present through a released window. */
    if (atomic_load(&started)) dm_gfx_suspend(1);
    dm_audio_suspend(focus!=1 || !next);
    dm_vk_set_window(next);
    pthread_mutex_lock(&state_lock);
    ANativeWindow *old=native_window; native_window=next;
    window_state=(dm_android_window_state){.focus=focus,.surface_available=next!=NULL,
        .width=(uint32_t)width,.height=(uint32_t)height,.density=density,.refresh_hz=hz,.thermal_status=-1};
    dm_android_window_state w=window_state;
    pthread_mutex_unlock(&state_lock);
    if (old) ANativeWindow_release(old);
    if (next && !atomic_load(&input_bound)) {
        dm_android_input_config input={.source="Android.SurfaceView.MotionEvent",.panel_width=1280,.panel_height=720,
            .source_ready=input_source_ready,.logical_npad_style=1,.logical_npad_id=0};
        int rc=dm_android_input_bind(&input);if(rc)return rc;atomic_store(&input_bound,1);
    }
    if (next) {
        /* 面板刷新率请求：游戏目标 30/60 帧，120Hz 面板只会白烧电。API 30+ 才有
         * ANativeWindow_setFrameRate，用 dlsym 取，缺失就跳过。DM_DISPLAY_HZ 可覆盖。
         * Display refresh hint: the game targets 30/60 fps; a 120 Hz panel just burns power.
         * ANativeWindow_setFrameRate exists on API 30+ only: resolve it by name and skip if
         * missing. DM_DISPLAY_HZ overrides (30..120). */
        static int (*set_frame_rate)(ANativeWindow *, float, int32_t);
        static int looked_up;
        if (!looked_up) { looked_up = 1;
            void *lib = dlopen("libandroid.so", RTLD_NOW|RTLD_LOCAL);
            if (lib) *(void **)(void *)&set_frame_rate = dlsym(lib, "ANativeWindow_setFrameRate");
        }
        if (set_frame_rate) {
            const char *fr = getenv("DM_DISPLAY_HZ");
            float target = fr ? strtof(fr, NULL) : 60.0f;
            if (target >= 30.0f && target <= 120.0f) {
                int rc = set_frame_rate(next, target, 0);   /* COMPATIBILITY_DEFAULT：让系统自适应 */
                LOG("DISPLAY_HZ 请求 %.0f Hz -> rc=%d", (double)target, rc);
            }
        }
    }
    if (!next || focus!=1) dm_android_input_cancel();
    dm_vk_set_display_hz((int)(hz+0.5f));
    dm_android_platform_status status; dm_android_platform_get_status(&status);
    if (status.bound) {int rc=dm_android_platform_update_window(&w);if(rc)return rc;}
    if (next && focus==1) dm_gfx_suspend(0);
    return 0;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeTouch(JNIEnv *env,jclass cls,jintArray ids,jfloatArray xs,jfloatArray ys,jboolean cancelled) {
    (void)cls;
    if (!atomic_load(&input_bound)) return ENODEV;
    if (cancelled) {dm_android_input_cancel();return 0;}
    if (!ids || !xs || !ys) return EINVAL;
    jsize n=(*env)->GetArrayLength(env,ids);
    if(n>16 || (*env)->GetArrayLength(env,xs)!=n || (*env)->GetArrayLength(env,ys)!=n)return EINVAL;
    jint pointer_ids[16];jfloat px[16],py[16];dm_android_pointer points[16];
    (*env)->GetIntArrayRegion(env,ids,0,n,pointer_ids);(*env)->GetFloatArrayRegion(env,xs,0,n,px);(*env)->GetFloatArrayRegion(env,ys,0,n,py);
    if((*env)->ExceptionCheck(env))return EINVAL;
    for(jsize i=0;i<n;++i)points[i]=(dm_android_pointer){.id=(uint32_t)pointer_ids[i],.x=px[i],.y=py[i]};
    return dm_android_input_publish_touch(points,(size_t)n);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeStart(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;
    if (!atomic_load(&configured) || !dm_android_native_window()) return ENODEV;
    if (atomic_exchange(&started,1)) return EALREADY;
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,8*1024*1024);
    pthread_t worker;int error=pthread_create(&worker,&attr,game_thread,NULL);pthread_attr_destroy(&attr);
    if (error) {atomic_store(&started,0);return error;} pthread_detach(worker);return 0;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeActions(JNIEnv *env,jclass cls,jlong buttons,jint lx,jint ly,jint rx,jint ry) {
    (void)env;(void)cls;return dm_android_input_publish_actions((uint64_t)buttons,lx,ly,rx,ry);
}
JNIEXPORT jstring JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeStatus(JNIEnv *env,jclass cls) {
    (void)cls;char text[384];
    pthread_mutex_lock(&state_lock);
    status_text(text,sizeof text);
    pthread_mutex_unlock(&state_lock);persist_status();return (*env)->NewStringUTF(env,text);
}
