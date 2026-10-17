"""Actual generated CV/lock code under controlled SDK-fastpath interleavings.
Host pthread operations are a deterministic scheduler, not a device race test.
The old publish-after-unlock sequence is a negative control.
"""
import argparse,ctypes as C,hashlib,json,re,subprocess,sys
from pathlib import Path

PREFIX=r'''
#include <stdint.h>
#include <stddef.h>
#define WAITERS_BIT 0x40000000u
#define R_TIMEOUT 0xea01u
#define R_CANCELLED 0xec01u
#define ETIMEDOUT 110
#define CLOCK_REALTIME 0
#define SW_TEST_HOOKS 1
struct timespec {int64_t tv_sec,tv_nsec;};
typedef struct thr {int prio,wkind,woke,cv;uint64_t waddr,wkey,wseq;uint32_t wtag;int64_t wres;struct thr *next;} thr_t;
static thr_t waiter,*g_wait;
static uint64_t g_wseq;
static uint32_t mutex_word,key_word;
static int G,violations,pending,mode,waits,saw_key;
static thr_t *cur(void){return &waiter;}
static int xblr_execution_failed(void){return 0;}
static int sw_detclock_enabled(void){return 0;}
static uint64_t guest_ns(void){return 0;}
static int clock_gettime(int c,struct timespec *t){(void)c;t->tv_sec=t->tv_nsec=0;return 0;}
static void pthread_mutex_lock(int *m){if(*m)violations++;*m=1;}
static void pthread_mutex_unlock(int *m){if(!*m)violations++;*m=0;}
static void pthread_cond_signal(int *c){(void)c;if(!G)violations++;}
static int scheduled_wait(void);
static int pthread_cond_wait(int *c,int *g){(void)c;(void)g;return scheduled_wait();}
static int pthread_cond_timedwait(int *c,int *g,const struct timespec *t){(void)c;(void)g;(void)t;return scheduled_wait();}
static int dispatch(int num,uint64_t *regs);
static void fast_reader(void){
 if(!G)violations++;
 uint32_t z=0;if(!__atomic_compare_exchange_n(&mutex_word,&z,0x2002,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))violations++;
 saw_key=key_word!=0;pending=saw_key;
 if(mode==1)mutex_word=0; /* Reader finishes before queued Signal acquires G. */
}
void (*sw_test_cv_after_release)(void)=fast_reader;
'''
SUFFIX=r'''
static int scheduled_wait(void){
 if(!G)violations++;G=0;++waits;
 if(mode==2&&waits==1){G=1;return 0;} /* Spurious wake cannot lose the real Signal. */
 if(pending){
  pending=0;uint64_t r[4]={(uintptr_t)&key_word,1,0,0};dispatch(0x1d,r);
  if(mutex_word==0x2002u||mutex_word==(0x2002u|WAITERS_BIT)){
   uint64_t u[4]={(uintptr_t)&mutex_word,0,0,0};dispatch(0x1b,u);
  }
  G=1;return 0;
 }
 G=1;return ETIMEDOUT; /* Watchdog for the negative control; never an infinite test. */
}
uint64_t run_case(int variant,uint64_t *out){
 waiter=(thr_t){.prio=20};g_wait=0;g_wseq=0;G=violations=pending=waits=saw_key=0;mode=variant;
 mutex_word=0x1001;key_word=0;
 uint64_t r[4]={(uintptr_t)&mutex_word,(uintptr_t)&key_word,0x1001,(uint64_t)-1};dispatch(0x1c,r);
 out[0]=mutex_word;out[1]=key_word;out[2]=g_wait!=0;out[3]=waiter.wkind;out[4]=G;out[5]=violations;out[6]=waits;out[7]=saw_key;
 return r[0];
}
'''

def function(text,signature):
    start=text.index(signature);brace=text.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]+'\n'

def main(sdk,out):
    root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'tools'))
    from prepare_services import prepare
    out.mkdir(parents=True,exist_ok=False);gen=out/'generated';gen.mkdir();prepare(root,gen)
    text=(gen/'hle_thread_ohos.c').read_text(encoding='utf-8');original=(root.parent/'host/hle_thread.c').read_text(encoding='utf-8')
    signatures=['static void wait_unlink(', 'static thr_t *pick(', 'static void wake(', 'static inline uint32_t ld32(', 'static inline void st32(', 'static inline int cas32(', 'static void lock_handoff(', 'static void lock_acquire_for(']
    helpers='';unchanged=[]
    for sig in signatures:
        a=function(text,sig);assert a==function(original,sig),sig;helpers+=a;unchanged.append(sig)
    helpers+=function(text,'static int64_t block_inner(thr_t *t, int kind, int64_t ns) {')
    helpers+='static int64_t block(thr_t *t,int kind,int64_t ns){return block_inner(t,kind,ns);}\n'
    start=text.index('    case 0x1B:');end=text.index('    case 0x34:',start);cases=text[start:end]
    assert cases==original[original.index('    case 0x1B:'):original.index('    case 0x34:')]
    body=PREFIX+helpers+'static int dispatch(int num,uint64_t *regs){switch(num){\n'+cases+'}return 0;}\n'+SUFFIX
    rows=[];commands=[]
    for name,flags in [('actual',[]),('old_order',['-DSW_TEST_CV_OLD_ORDER=1'])]:
        c=out/(name+'.c');c.write_text(body,encoding='utf-8');dll=out/(name+'.dll')
        argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run_case',*flags,str(c),str(root/'tests/windows_gate_support.c'),'-o',str(dll)]
        p=subprocess.run(argv,capture_output=True,text=True);commands.append(argv);(out/(name+'.log')).write_text(p.stdout+p.stderr);assert not p.returncode,p.stderr
        lib=C.CDLL(str(dll));lib.run_case.argtypes=[C.c_int,C.POINTER(C.c_uint64)];lib.run_case.restype=C.c_uint64
        for mode in range(3):
            report=(C.c_uint64*8)();actual=lib.run_case(mode,report)
            if name=='actual':assert actual==0 and list(report)==[0x1001,0,0,0,0,0,2 if mode==2 else 1,1],(name,mode,actual,list(report))
            else:assert actual==0xea01 and report[7]==0 and report[5]==0,(name,mode,actual,list(report))
            rows.append({'variant':name,'schedule':mode,'return':actual,'report':list(report)})
    result={'result':'PASS','scope':__doc__,'current_cases':3,'negative_cases':3,'unchanged_original_functions':unchanged,'cv_svc_cases_identical_to_original':True,'generated_sha256':hashlib.sha256(text.encode()).hexdigest(),'rows':rows}
    (out/'commands.json').write_text(json.dumps(commands,indent=2));(out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:result[k] for k in ('result','current_cases','negative_cases','cv_svc_cases_identical_to_original')}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
