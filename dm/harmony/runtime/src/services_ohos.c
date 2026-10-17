#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hle_ohos_compat.h"
#include "execution_internal.h"
#include "xblr_interaction.h"
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <sys/uio.h>
#include <sys/mman.h>
module_t g_mod[MAX_MOD];
int g_nmod;
static _Atomic uint64_t prof_time[PROF_N],prof_bytes[PROF_N],frames;
static void (*write_prepare)(void *,size_t);
static void (*ui_observers[16])(const char*,const char*,const char*);
static unsigned ui_count;
#include "guest_read_ohos.inc"
#include "world_tick_ohos.inc"
#include "padmode_boot_ohos.inc"
#include "ui_touch_ohos.inc"
#include "interaction_ohos.inc"
#include "world_memory_ohos.inc"
#include "world_geometry_ohos.inc"
#include "world_target_ohos.inc"
#include "world_plan_ohos.inc"
#include "world_store_ohos.inc"
#include "world_go_ohos.inc"
#include "world_queue_ohos.inc"
#include "ui_frontend_ohos.inc"
static char configured_romfs[PATH_MAX];
static int flat_romfs;
const char *xblr_romfs_rest(const char *root,const char *rest) {
    if(!flat_romfs||strcmp(root,configured_romfs))return rest;
    const char *p=rest;while(*p=='/')++p;
    if(!strncmp(p,"data",4)&&(p[4]=='/'||p[4]==0))return p+4;
    return *p?NULL:rest;
}
int xblr_romfs_is_root(const char *hostpath,const char *guestpath) {
    if(!flat_romfs||strcmp(hostpath,configured_romfs))return 0;
    const char *p=strchr(guestpath,':');if(!p)return 0;++p;while(*p=='/')++p;return !*p;
}
const char *xblr_romfs_name(const char *parent,const char *name) {
    if(flat_romfs&&!strcmp(parent,configured_romfs)&&!strcmp(name,"build.config"))return ".build.config";
    return name;
}
int xblr_fs_guest_name_matches(const char *hostpath,const char *guestpath) {
    /* Horizon accepts directory paths with trailing separators. Preserve the
     * original host's component normalization without APFS alias/xattr logic. */
    const char *end=guestpath;while(*end)++end;
    while(end>guestpath&&end[-1]=='/')--end;
    const char *start=end;while(start>guestpath&&start[-1]!='/'&&start[-1]!=':')--start;
    const char *physical=hostpath;for(const char *p=hostpath;*p;++p)if(*p=='/')physical=p+1;
    size_t n=(size_t)(end-start);if(!n||n>=0x301)return 0;
    for(size_t i=0;i<n;++i)if(!physical[i]||physical[i]!=start[i])return 0;
    return physical[n]==0;
}
int xblr_fs_roots_disjoint(const char *a,const char *b) {
    /* Inputs are canonical absolute directory names from realpath. Include the
     * transaction siblings in this check before mounting can remove stale work. */
    if((a[0]=='/'&&!a[1])||(b[0]=='/'&&!b[1]))return 0;
    while(*a&&*b&&*a==*b){++a;++b;}
    if(*a&&*b)return 1;
    return *a?*a!='/':*b&&*b!='/';
}
void sw_prof_add(int i,uint64_t n,uint64_t b) {if(i>=0&&i<PROF_N){atomic_fetch_add(&prof_time[i],n);atomic_fetch_add(&prof_bytes[i],b);}}
void sw_prof_mark(const char *n) {LOG("stage %s",n);}
void crash_watch_frame(void) {atomic_fetch_add(&frames,1);}
void uimsg_subscribe(void (*fn)(const char*,const char*,const char*)) {if(ui_count>=16)die("UI observer overflow");ui_observers[ui_count++]=fn;}
void sw_guest_write_prepare(void *p,size_t n) {if(write_prepare)write_prepare(p,n);}
void sw_guest_write_prep_register(void (*fn)(void*,size_t)) {write_prepare=fn;}
void sw_backtrace(uint64_t fp) {LOG("guest frame pointer=%#llx",(unsigned long long)fp);}
void hlecount_svc(int n) {(void)n;} /* diagnostic counter: execution report already counts every gate */
uint64_t svc_host_call(int n,uint64_t *r) {(void)n;(void)r;return 0;} /* no private iOS cuts installed */
module_t *module_of(const void *p) {uintptr_t a=(uintptr_t)p;for(int i=0;i<g_nmod;++i)if(a>=(uintptr_t)g_mod[i].base&&a-(uintptr_t)g_mod[i].base<g_mod[i].size)return &g_mod[i];return NULL;}
extern void *xblr_module_named_symbol(const char *,const char *);
extern int xblr_execution_has_started(void);
void *module_sym(module_t *m,const char *n) {return m?xblr_module_named_symbol(m->name,n):NULL;}
static xblr_nvn_resolver nvn_resolver;
typedef void (*xblr_present_fn)(void*,void*,int);
static _Atomic(xblr_present_fn) nvn_present_original;
typedef void *(*xblr_proc_fn)(const void*,const char*);
static _Atomic(xblr_proc_fn) nvn_proc_original;
static void *nvn_observe_proc(const char *name,void *p);
static void *nvn_proc_observed(const void *device,const char *name) {
    return nvn_observe_proc(name,atomic_load(&nvn_proc_original)(device,name));
}
static void nvn_present_observed(void *queue,void *window,int index) {
    atomic_load(&nvn_present_original)(queue,window,index);crash_watch_frame();
}
static void *nvn_observe_proc(const char *name,void *p) {
    if(!p)return NULL;
    if(!strcmp(name,"nvnQueuePresentTexture")){atomic_store(&nvn_present_original,(xblr_present_fn)p);return nvn_present_observed;}
    if(!strcmp(name,"nvnDeviceGetProcAddress")){atomic_store(&nvn_proc_original,(xblr_proc_fn)p);return nvn_proc_observed;}
    return p;
}
void xblr_pump_ui_touch(void) {xblr_ui_touch_pump();xblr_world_pump();}
int xblr_graphics_configured(void) {return nvn_resolver!=NULL;}
int xblr_set_nvn_resolver(xblr_nvn_resolver resolver) {
    if(!resolver)return XBLR_EXEC_ARGUMENT;
    if(xblr_execution_has_started())return XBLR_EXEC_BUSY;
    nvn_resolver=resolver;return 0;
}
HLE_FORCE("nvnBootstrapLoader","Harmony graphics resolver",void *,h_nvn_bootstrap,const char *name) {
    if(!nvn_resolver)xblr_graphics_fatal(NULL,"NVN resolver not configured");
    void *p=nvn_resolver(name);if(!p)xblr_graphics_fatal(NULL,name);
    return nvn_observe_proc(name,p);
}
int xblr_set_touches(const xblr_touch *t,uint32_t n) {
    if(n>16||(n&&!t))return XBLR_EXEC_ARGUMENT;
    uint32_t seen=0;for(uint32_t i=0;i<n;++i) {
        if(t[i].id>=16||(seen&(1u<<t[i].id))||!isfinite(t[i].x)||!isfinite(t[i].y)||t[i].x<0||t[i].x>1||t[i].y<0||t[i].y>1)return XBLR_EXEC_ARGUMENT;
        seen|=1u<<t[i].id;
    }
    sw_touch_set(t,(int)n);return 0;
}
int xblr_set_focus(int st) {if(st<1||st>3)return XBLR_EXEC_ARGUMENT;world_set_focus(st==1);if(st!=1)xblr_ui_touch(3,0,0);int failed=sw_audio_suspend(st!=1);sw_focus_set(st);return failed?XBLR_EXEC_HOST_FAILURE:0;}
uint64_t xblr_audio_render_calls(void) {return sw_audio_render_calls();}
static xblr_keyboard_handler keyboard_handler;
static void *keyboard_userdata;
int xblr_set_keyboard_handler(xblr_keyboard_handler handler,void *userdata) {
    if(xblr_execution_has_started())return XBLR_EXEC_BUSY;
    keyboard_handler=handler;keyboard_userdata=userdata;return 0;
}
int front_text_input(const char *title,const char *subtitle,const char *guide,const char *initial,int max,char *out,int bytes) {
    xblr_guest_thread *t=xblr_current_thread();
    if(!keyboard_handler) {
        if(t&&t->report)snprintf(t->report->symbol,sizeof t->report->symbol,"soft keyboard handler not configured");
        xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);
    }
    if(!out||bytes<1)xblr_execution_stop(XBLR_EXEC_ARGUMENT);
    memset(out,0,(size_t)bytes);int result=keyboard_handler(keyboard_userdata,title,subtitle,guide,initial,max,out,bytes);
    if(result<0||!memchr(out,0,(size_t)bytes))xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);
    return result;
}

/* Existing save transaction semantics, using an actual durable copy and atomic
 * Linux directory exchange. No destructive fallback if RENAME_EXCHANGE fails.
 * These routines execute only after the app explicitly supplies a save root.
 */
static int copy_tree(const char *from,const char *to,unsigned depth) {
    if(depth>32){errno=ELOOP;return -1;}
    struct stat st;if(lstat(from,&st))return -1;
    if(S_ISDIR(st.st_mode)) {
        if(mkdir(to,0700))return -1;
        DIR *d=opendir(from);if(!d)return -1;int result=0;struct dirent *e;
        /* Save commits run on SDK-sized guest stacks. Recursive PATH_MAX
         * arrays consume 8 KiB per directory; keep only small frames on stack. */
        char *paths=malloc(2u*PATH_MAX);if(!paths){closedir(d);errno=ENOMEM;return -1;}
        char *a=paths,*b=paths+PATH_MAX;
        for(;;) {
            /* readdir uses NULL for both EOF and errors. A truncated directory
             * must never be accepted as a complete save transaction snapshot. */
            errno=0;e=readdir(d);
            if(!e){if(errno)result=-1;break;}
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
            if(snprintf(a,PATH_MAX,"%s/%s",from,e->d_name)>=PATH_MAX||snprintf(b,PATH_MAX,"%s/%s",to,e->d_name)>=PATH_MAX){errno=ENAMETOOLONG;result=-1;break;}
            if(copy_tree(a,b,depth+1)){result=-1;break;}
        }
        int saved=errno;free(paths);
        if(closedir(d)&&!result){result=-1;saved=errno;}
        if(!result){int fd=open(to,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0){result=-1;saved=errno;}else {
            result=fsync(fd);saved=errno;if(close(fd)&&!result){result=-1;saved=errno;}
        }}errno=saved;return result;
    }
    if(!S_ISREG(st.st_mode)){errno=ENOTSUP;return -1;}
    int in=open(from,O_RDONLY|O_NOFOLLOW);if(in<0)return -1;
    int out=open(to,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(out<0){int e=errno;close(in);errno=e;return -1;}
    unsigned char *buf=malloc(65536);int result=buf?0:-1;
    if(!buf)errno=ENOMEM;
    while(!result) {ssize_t n=read(in,buf,65536);if(n<0){if(errno==EINTR)continue;result=-1;break;}if(!n)break;
        ssize_t done=0;while(done<n){ssize_t w=write(out,buf+done,(size_t)(n-done));if(w<0&&errno==EINTR)continue;if(w<=0){if(!w)errno=EIO;result=-1;break;}done+=w;}}
    if(!result)result=fsync(out);int saved=errno;free(buf);
    if(close(out)&&!result){result=-1;saved=errno;}
    if(close(in)&&!result){result=-1;saved=errno;}
    errno=saved;return result;
}
int xblr_clone_tree(const char *from,const char *to,int flags) {if(flags){errno=EINVAL;return -1;}return copy_tree(from,to,0);}
static int sync_parent(const char *path) {
    char p[1024];if(strlcpy(p,path,sizeof p)>=sizeof p){errno=ENAMETOOLONG;return -1;}
    char *slash=strrchr(p,'/');if(!slash||slash==p){errno=EINVAL;return -1;}*slash=0;
    int fd=open(p,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return -1;int r=fsync(fd),e=errno;close(fd);errno=e;return r;
}
int xblr_exchange_save(const char *from,const char *to,int flags) {
    if(flags!=2){errno=EINVAL;return -1;}
#ifdef __NR_renameat2
    if(syscall(__NR_renameat2,AT_FDCWD,from,AT_FDCWD,to,2))return -1;
    /* The exchange already committed. If directory fsync fails, roll back the
     * exchange before reporting failure so old saved bytes remain canonical. */
    if(sync_parent(to)) {int e=errno;if(syscall(__NR_renameat2,AT_FDCWD,from,AT_FDCWD,to,2))die("save exchange rollback failed");(void)sync_parent(to);errno=e;return -1;}
    return 0;
#else
    (void)from;(void)to;errno=ENOTSUP;return -1;
#endif
}
int xblr_configure_game_paths(const char *rom,const char *save,const char *temp) {
    if(xblr_execution_has_started())return XBLR_EXEC_BUSY;
    /* Existing overlay was validated against old roots. Clear it explicitly
     * before replacing roots rather than silently retaining an unsafe mount. */
    if(getenv("SW_ROMFS_OVERLAY"))return XBLR_EXEC_ARGUMENT;
    if(!rom||!save||!temp||rom[0]!='/'||save[0]!='/'||temp[0]!='/')return XBLR_EXEC_ARGUMENT;
    char r[PATH_MAX],s[PATH_MAX],t[PATH_MAX];
    if(!realpath(rom,r)||!realpath(save,s)||!realpath(temp,t))return XBLR_EXEC_ARGUMENT;
    struct stat st;
    for(unsigned i=0;i<3;++i){const char *p=i==0?r:i==1?s:t;if(stat(p,&st)||!S_ISDIR(st.st_mode))return XBLR_EXEC_ARGUMENT;}
    /* Original mount records have 1024-byte names. Never accept a path that
     * would truncate, or a .work/.new sibling overlapping resources/temp. */
    if(strlen(r)>=1024||strlen(t)>=1024||strlen(s)+5>=1024)return XBLR_EXEC_ARGUMENT;
    char work[1024],next[1024];snprintf(work,sizeof work,"%s.work",s);snprintf(next,sizeof next,"%s.new",s);
    const char *paths[5]={r,s,t,work,next};
    for(unsigned i=0;i<5;++i)for(unsigned j=i+1;j<5;++j)
        if(!xblr_fs_roots_disjoint(paths[i],paths[j]))return XBLR_EXEC_ARGUMENT;
    char config[PATH_MAX],archive[PATH_MAX];
    if(snprintf(config,sizeof config,"%s/data/.build.config",r)>=(int)sizeof config)return XBLR_EXEC_ARGUMENT;
    int flat=access(config,R_OK)!=0;
    if(flat) {
        if(snprintf(config,sizeof config,"%s/build.config",r)>=(int)sizeof config||
           snprintf(archive,sizeof archive,"%s/archive_manifest.psv",r)>=(int)sizeof archive)return XBLR_EXEC_ARGUMENT;
        if(stat(config,&st)||!S_ISREG(st.st_mode)||stat(archive,&st)||!S_ISREG(st.st_mode))return XBLR_EXEC_ARGUMENT;
    }
    if(setenv("SW_ROMFS",r,1)||setenv("SW_SAVE",s,1)||setenv("SW_TEMP",t,1))return XBLR_EXEC_HOST_FAILURE;
    strlcpy(configured_romfs,r,sizeof configured_romfs);flat_romfs=flat;
    LOG("configured RomFS layout=%s (read-only path view)",flat?"flat import":"native data directory");
    return 0;
}
int xblr_configure_game_overlay(const char *absolute_root) {
    if(xblr_execution_has_started())return XBLR_EXEC_BUSY;
    if(!absolute_root)return unsetenv("SW_ROMFS_OVERLAY")?XBLR_EXEC_HOST_FAILURE:0;
    if(!configured_romfs[0]||absolute_root[0]!='/'||strchr(absolute_root,':'))return XBLR_EXEC_ARGUMENT;
    char root[PATH_MAX];if(!realpath(absolute_root,root)||strlen(root)>=900||strchr(root,':'))return XBLR_EXEC_ARGUMENT;
    const char *save=getenv("SW_SAVE"),*temp=getenv("SW_TEMP");if(!save||!temp)return XBLR_EXEC_ARGUMENT;
    char work[1024],next[1024];snprintf(work,sizeof work,"%s.work",save);snprintf(next,sizeof next,"%s.new",save);
    const char *base[5]={configured_romfs,save,temp,work,next};
    for(unsigned i=0;i<5;i++)if(!xblr_fs_roots_disjoint(root,base[i]))return XBLR_EXEC_ARGUMENT;
    /* gen_romfs_overlay emit packs logical layouts into CASC, not directories.
     * The importer owns index/hash validation; this API validates the physical
     * mount shape. A marker alone is not a completeness certificate. */
    static const char *required[]={"ctrl_touch","touch_strings.tsv","data/.build.config"};
    for(unsigned i=0;i<sizeof required/sizeof *required;i++){
        char path[PATH_MAX],resolved[PATH_MAX];struct stat st;
        if(snprintf(path,sizeof path,"%s/%s",root,required[i])>=(int)sizeof path||
           !realpath(path,resolved)||strncmp(resolved,root,strlen(root))||resolved[strlen(root)]!='/'||
           stat(resolved,&st)||access(resolved,R_OK)||!S_ISREG(st.st_mode)||st.st_size<=0)return XBLR_EXEC_ARGUMENT;
    }
    char data[PATH_MAX];snprintf(data,sizeof data,"%s/data",root);DIR *d=opendir(data);if(!d)return XBLR_EXEC_ARGUMENT;
    struct dirent *entry;unsigned archives=0;int invalid=0;
    while((entry=readdir(d))){size_t n=strlen(entry->d_name);if(n<5||strcmp(entry->d_name+n-5,".data"))continue;
        char path[PATH_MAX],resolved[PATH_MAX];struct stat st;
        if(snprintf(path,sizeof path,"%s/%s",data,entry->d_name)>=(int)sizeof path||!realpath(path,resolved)||
           strncmp(resolved,root,strlen(root))||resolved[strlen(root)]!='/'||stat(resolved,&st)||
           !S_ISREG(st.st_mode)||st.st_size<=0||access(resolved,R_OK)){invalid=1;break;}++archives;
    }
    closedir(d);if(invalid||archives!=1)return XBLR_EXEC_ARGUMENT;
    return setenv("SW_ROMFS_OVERLAY",root,1)?XBLR_EXEC_HOST_FAILURE:0;
}
