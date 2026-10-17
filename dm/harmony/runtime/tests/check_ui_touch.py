"""Production UI tap pump with fake guest root and frame clock; no game execution."""
import argparse,ctypes as C,json,subprocess
from pathlib import Path
PREFIX=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#define PTHREAD_MUTEX_INITIALIZER 0
#define XBLR_EXEC_ARGUMENT 40
#define XBLR_EXEC_BUSY 47
int _fltused=0;
#define SYS_gettid 178
static long syscall(int n){return 7;}
static unsigned receipt_lines;
#define LOG(...) (++receipt_lines)
#define isfinite(x) ((x)==(x)&&(x)<=3.402823466e38F&&(x)>=-3.402823466e38F)
typedef int pthread_mutex_t;
void *memcpy(void*,const void*,size_t);void *memset(void*,int,size_t);
static int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *(unsigned char*)a-*(unsigned char*)b;}
static int violations,logs;static uint64_t actions[64];static int xs[64],ys[64];
static void pthread_mutex_lock(int*p){if(*p)violations++;*p=1;}
static void pthread_mutex_unlock(int*p){if(!*p)violations++;*p=0;}
typedef struct {int is_main;} xblr_guest_thread;
static xblr_guest_thread thread={1};static xblr_guest_thread*xblr_current_thread(void){return &thread;}
typedef struct {char name[16];uint8_t*base;uint64_t size;} module_t;
static uint8_t memory[0x3200000];static uint64_t root[8],vt[8];
static module_t g_mod[1]={{"main",memory,sizeof memory}};static int g_nmod=1;
static _Atomic uint64_t frames;
typedef void *(*xblr_nvn_resolver)(const char*);
static void crash_watch_frame(void){frames++;}
static uint64_t xblr_invoke_guest(void*fn,const uint64_t*a,uint32_t n){
 (void)fn;if(!thread.is_main||n!=2||a[0]!=(uintptr_t)root)violations++;
 uint64_t*m=(void*)(uintptr_t)a[1],*p=(void*)(uintptr_t)m[2];
 if(logs<64){actions[logs]=m[1];xs[logs]=(int)(p[0]>>32);ys[logs]=(int)p[1];logs++;}return 0;
}
'''
SUFFIX=r'''
static int present_calls;
static void fake_present(void*q,void*w,int i){if(q!=(void*)1||w!=(void*)2||i!=3)violations++;present_calls++;}
static void *fake_proc(const void*d,const char*n){if(d!=(void*)4)violations++;return !strcmp(n,"nvnQueuePresentTexture")?fake_present:0;}
static void step(void){frames++;xblr_ui_touch_pump();}
int run(void){
 xblr_present_fn present=nvn_observe_proc("nvnQueuePresentTexture",fake_present);present((void*)1,(void*)2,3);
 xblr_proc_fn proc=nvn_observe_proc("nvnDeviceGetProcAddress",fake_proc);
 present=proc((void*)4,"nvnQueuePresentTexture");present((void*)1,(void*)2,3);
 if(present_calls!=2||frames!=2||violations||proc((void*)4,"unknown")||nvn_observe_proc("other",fake_present)!=fake_present)return 14;
 if(receipt_lines||xblr_set_ui_touch_receipt_budget(5)!=40)return 15;
 xblr_set_ui_touch_receipt_budget(4);
 uint64_t r=(uintptr_t)root;root[0]=(uintptr_t)vt;vt[5]=123;
 int w=1920,h=1080;memcpy(memory+0x2f05458,&r,8);memcpy(memory+0x211da38,&w,4);memcpy(memory+0x211da3c,&h,4);
 if(xblr_ui_touch(4,0,0)!=40||xblr_ui_touch(0,-1,0)!=40)return 1;
 if(xblr_ui_touch(0,.5f,.25f)||xblr_ui_touch(2,.5f,.25f))return 2;
 if(xblr_ui_touch(0,0,0)!=47)return 3;
 thread.is_main=0;step();if(logs)return 4;thread.is_main=1;
 step();xblr_ui_touch_pump();if(logs!=1)return 5;step();if(logs!=2)return 6;
 step();if(logs!=4||actions[3]!=ui_hash("LeftMouseButtonDown"))return 7;
 step();if(logs!=6||actions[4]!=ui_hash("MouseMove")||actions[5]!=ui_hash("LeftMouseButtonUp")||xs[5]!=960||ys[5]!=270)return 8;
 if(ui_touch.phase||violations)return 9;
 xblr_ui_touch(0,1,1);xblr_ui_touch(2,1,1);xblr_ui_touch(3,0,0);step();if(logs!=6||ui_touch.phase)return 10;
 xblr_ui_touch(0,1,1);xblr_ui_touch(2,1,1);step();step();step();xblr_ui_touch(3,0,0);step();
 if(actions[logs-1]!=ui_hash("LeftMouseButtonUp")||xs[logs-1]!=1919||ys[logs-1]!=1079||ui_touch.phase)return 11;
 xblr_ui_touch(0,.1f,.1f);step();xblr_ui_touch(1,.2f,.2f);xblr_ui_touch(2,.2f,.2f);int before=logs;step();step();if(logs!=before+2)return 12;step();step();
 if(!receipt_lines||receipt_lines>48||ui_diag_started!=4)return 16;
 unsigned saved=receipt_lines;xblr_set_ui_touch_receipt_budget(0);ui_trace(0,"disabled",0,0,0,0,0,0);
 if(receipt_lines!=saved)return 17;
 xblr_set_ui_touch_receipt_budget(4);xblr_ui_touch(0,.5f,.5f);xblr_ui_touch(3,0,0);step();
 if(receipt_lines!=saved||ui_diag_started!=4)return 18;
 return violations?13:0;
}
'''
def main(sdk,out):
    root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
    services=(root/'src/services_ohos.c').read_text()
    resolver=services[services.index('static xblr_nvn_resolver nvn_resolver;'):services.index('int xblr_graphics_configured(void)')]
    # Resolver extraction also includes the HID wrapper; world semantics have
    # their own production queue test, while this unit exercises only UI taps.
    code=PREFIX+'static void xblr_world_pump(void){}\n'+(root/'src/ui_touch_ohos.inc').read_text()+resolver+SUFFIX
    c=out/'test.c';c.write_text(code);dll=out/'test.dll'
    argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',str(c),str(root/'tests/windows_gate_support.c'),'-o',str(dll)]
    p=subprocess.run(argv,capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert not p.returncode,p.stderr
    result=C.CDLL(str(dll)).run();assert result==0,result
    r={'result':'PASS','scope':__doc__,'checks':['argument validation','busy does not overwrite tap','main thread only','one pump per present','two hover frames then down then next-frame move before up','cancel before down','cancel after down releases','coordinate edge clamp','move resets hover','bootstrap and device proc present forwarding','unknown and null proc passthrough','receipts default off and invalid budget','12 lines per tap and four lifetime taps','disable immediately and no lifetime rearm']}
    (out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
