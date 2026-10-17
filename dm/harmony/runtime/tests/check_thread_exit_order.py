"""Production thread exit/main/finish functions with a modeled stack switch.
Join immediately recycles the guest stack at wake publication. This verifies
publication ordering, not ARM64 longjmp itself or a live device scheduler.
"""
import argparse,ctypes as C,json,subprocess,sys
from pathlib import Path
from check_condition_interleaving import function

PREFIX=r'''
#include <stdint.h>
#include <stddef.h>
typedef struct thr {uint64_t entry,arg,stack_top,exit_ns;uint32_t exit_tick;int started,exited,closed,qos,wkind,cv;struct thr *next;} thr_t;
static thr_t worker,joiner,*g_wait,*current;
static int G,g_keythr,on_guest,violations,finishes,recycles,frees,mode;
static uint32_t (*sw_thread_tick_fn)(void);
static uint64_t now_ns(void){return 123;}
static void *pthread_getspecific(int key){(void)key;return current;}
static int pthread_setspecific(int key,void *p){(void)key;current=p;return 0;}
static void pthread_mutex_lock(int *p){if(*p)violations++;*p=1;}
static void pthread_mutex_unlock(int *p){if(!*p)violations++;*p=0;}
static int pthread_cond_wait(int *cv,int *p){(void)cv;(void)p;violations++;return 0;}
static void wake(thr_t *t,int64_t rc){(void)rc;if(!G||on_guest||!worker.exited)violations++;t->wkind=0;g_wait=0;recycles++;}
static void thr_free(thr_t *t){if(!G||on_guest||!t->exited)violations++;frees++;}
static void thread_finish(thr_t *t);
static void thread_exit(thr_t *t);
static void xblr_exit_guest_thread(void){
 /* The model makes the native return boundary explicit. Real production uses
  * longjmp to the setjmp inside xblr_run_guest_thread / xblr_execute_startup. */
 if(recycles)violations++;on_guest=0;
}
static void xblr_run_guest_thread(uint64_t entry,uint64_t arg,uint64_t stack){
 (void)entry;(void)arg;(void)stack;on_guest=1;
 if(mode!=2)thread_exit(current);else on_guest=0; /* STOP cleanup also returns native. */
 if(on_guest)violations++;
}
'''
SUFFIX=r'''
int run_case(int m,uint64_t *out){
 mode=m;G=on_guest=violations=finishes=recycles=frees=0;
 worker=(thr_t){.started=1,.closed=m==1};joiner=(thr_t){.wkind=4};g_wait=&joiner;current=0;
 thread_main(&worker);
 out[0]=worker.exited;out[1]=recycles;out[2]=frees;out[3]=current!=0;out[4]=G;out[5]=violations;
 return violations==0&&worker.exited&&recycles==1&&frees==(m==1)&&!current&&!G;
}
int run_main_exit(uint64_t *out){
 G=violations=recycles=frees=0;worker=(thr_t){.started=1};joiner=(thr_t){.wkind=4};g_wait=&joiner;current=&worker;on_guest=1;
 thread_exit(&worker); /* return stands for restored main native setjmp */
 xblr_thread_finish_current();
 out[0]=worker.exited;out[1]=recycles;out[2]=violations;
 return !violations&&worker.exited&&recycles==1&&!current&&!G;
}
'''

def main(sdk,out):
    root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'tools'))
    from prepare_services import prepare
    out.mkdir(parents=True,exist_ok=False);gen=out/'generated';gen.mkdir();prepare(root,gen)
    text=(gen/'hle_thread_ohos.c').read_text(encoding='utf-8')
    signatures=['static void thread_finish(thr_t *t) {','static void thread_exit(thr_t *t) {','void xblr_thread_finish_current(void) {','static void *thread_main(void *p) {']
    body=PREFIX+''.join(function(text,s) for s in signatures)+SUFFIX
    # Main's restored-stack cleanup must be present only for explicit ExitThread.
    execution=(root/'src/execution.c').read_text(encoding='utf-8');assert 'if(escaped==2)xblr_thread_finish_current();' in execution
    rows=[];commands=[]
    for name,code in [('actual',body),('early_publish_negative',body.replace('(void)t;xblr_exit_guest_thread();','thread_finish(t);xblr_exit_guest_thread();'))]:
        c=out/(name+'.c');c.write_text(code,encoding='utf-8');dll=out/(name+'.dll')
        argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run_case,/export:run_main_exit',str(c),str(root/'tests/windows_gate_support.c'),'-o',str(dll)]
        p=subprocess.run(argv,capture_output=True,text=True);commands.append(argv);(out/(name+'.log')).write_text(p.stdout+p.stderr);assert not p.returncode,p.stderr
        lib=C.CDLL(str(dll));lib.run_case.argtypes=[C.c_int,C.POINTER(C.c_uint64)];lib.run_case.restype=C.c_int;lib.run_main_exit.argtypes=[C.POINTER(C.c_uint64)]
        for mode in range(3):
            outrow=(C.c_uint64*6)();got=lib.run_case(mode,outrow)
            assert got==(1 if name=='actual' or mode==2 else 0),(name,mode,list(outrow))
            rows.append({'variant':name,'mode':mode,'pass':bool(got),'report':list(outrow)})
        outrow=(C.c_uint64*3)();got=lib.run_main_exit(outrow);assert got==(name=='actual'),(name,list(outrow))
        rows.append({'variant':name,'mode':'main explicit exit','pass':bool(got),'report':list(outrow)})
    result={'result':'PASS','scope':__doc__,'actual_cases':4,'negative_control':'early publication lets join recycle guest stack before native switch; rejected for worker and main exit','rows':rows}
    (out/'commands.json').write_text(json.dumps(commands,indent=2));(out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({'result':'PASS','actual_cases':4,'negative_control':'rejected'}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
