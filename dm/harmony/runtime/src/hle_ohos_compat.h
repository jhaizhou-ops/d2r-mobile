// Existing HLE source's platform registration adapter; game function bodies are unchanged.
#ifndef XBLR_HLE_OHOS_COMPAT_H
#define XBLR_HLE_OHOS_COMPAT_H
#include "xblr_loader.h"
#include "xblr_execution.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#define LOG(...) do { fprintf(stderr,"[xblr] " __VA_ARGS__);fputc('\n',stderr); } while(0)
#define die(...) do { LOG("FATAL " __VA_ARGS__);abort(); } while(0)
struct xblr_hle_flags { int thr,svc,fs,movie,dbg,imports,bootprof; };
extern struct xblr_hle_flags g_flag;
extern uint64_t g_t0_ns;
uint64_t now_ns(void);
void *guest_sym(const char *name);
typedef uint32_t Result;
#define RES_OK 0u
#define RES_FAIL 0x2A2u
#define TARGET_OS_IPHONE 0
enum {PROF_FS_READ,PROF_WAIT_LOCK,PROF_WAIT_CV,PROF_WAIT_ADDR,PROF_WAIT_SYNC,PROF_SLEEP,PROF_MAIN_WAIT,PROF_GFX_PRESENT,PROF_GFX_SHADER,PROF_TACT,PROF_N};
void sw_prof_add(int id,uint64_t ns,uint64_t bytes);
void sw_prof_mark(const char *name);
uint64_t guest_ns(void);
int sw_detclock_enabled(void);
void sw_guest_clock_advanced(void);
uint64_t sw_guest_frame(void);
void sw_guest_frame_tick(void);
uint64_t sw_entropy64(void);
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason,int on);
void *objmap_get(const void *guest);
void objmap_put(const void *guest,void *host);
void *objmap_del(const void *guest);
void hle_bind_system_event(void *event,int autoclear);
void hle_signal_system_event(void *event);
void sw_guest_write_prepare(void *addr,size_t bytes);
void sw_guest_write_prep_register(void (*fn)(void *,size_t));
void crash_watch_frame(void);
void xblr_pump_ui_touch(void);
void xblr_world_note_stick(int32_t lx,int32_t ly);
uint64_t xblr_pump_ui_frontend(void);
void uimsg_subscribe(void (*fn)(const char*,const char*,const char*));
int xblr_clone_tree(const char *from,const char *to,int flags);
int xblr_exchange_save(const char *from,const char *to,int flags);
void xblr_run_guest_thread(uint64_t entry,uint64_t arg,uint64_t sp);
void xblr_exit_guest_thread(void);
int xblr_execution_failed(void);
void xblr_guest_thread_init(void);
void sw_thread_enter(uint64_t entry,uint64_t arg,uint64_t sp);
uint32_t svc_thread_main_init(void);
int svc_thread(int num,uint64_t *regs);
typedef struct {char name[16];uint8_t *base;size_t size;} module_t;
#define MAX_MOD 8
extern module_t g_mod[MAX_MOD];
extern int g_nmod;
module_t *module_of(const void *address);
void *module_sym(module_t *module,const char *name);
void sw_backtrace(uint64_t fp);
void hlecount_svc(int number);
uint64_t svc_host_call(int number,uint64_t *regs);
uint64_t svc_dispatch(int number,uint64_t *regs,uint64_t original_pc);
int sw_opmode(void);
void sw_focus_set(int focus);
void sw_touch_set(const xblr_touch *touches,int count);
int sw_audio_suspend(int suspended);
uint64_t sw_audio_render_calls(void);
int front_text_input(const char *title,const char *subtitle,const char *guide,const char *initial,int maximum_chars,char *output,int output_bytes);
const char *xblr_romfs_rest(const char *mount_root,const char *guest_path);
const char *xblr_romfs_name(const char *parent,const char *physical_name);
int xblr_romfs_is_root(const char *hostpath,const char *guestpath);
int xblr_fs_guest_name_matches(const char *hostpath,const char *guestpath);
int xblr_graphics_configured(void);
#define HLE(mangled,ret,cname,...) \
    static ret cname(__VA_ARGS__); \
    __attribute__((used,section("xblr_hle"),aligned(8))) \
    static const xblr_hle_binding xblr_hle_##cname={mangled,(uintptr_t)cname,0}; \
    static ret cname(__VA_ARGS__)
#define HLE_FORCE(mangled,why,ret,cname,...) \
    static ret cname(__VA_ARGS__); \
    __attribute__((used,section("xblr_hle"),aligned(8))) \
    static const xblr_hle_binding xblr_hle_##cname={mangled,(uintptr_t)cname,1}; \
    static ret cname(__VA_ARGS__)
#endif
