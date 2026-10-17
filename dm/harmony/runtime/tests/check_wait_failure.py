"""Exercise generated WaitSynchronization and block_inner C with scheduled wakes.

Uses exact production function bodies. Deterministic condition-variable events
stand in for pthread scheduling; this is not a Harmony concurrency/device test.
"""
import argparse, ctypes as C, hashlib, json, re, subprocess, sys
from pathlib import Path

def main(sdk, out):
    root=Path(__file__).resolve().parents[1]
    sys.path.insert(0,str(root/'tools'))
    from prepare_services import prepare
    out.mkdir(parents=True,exist_ok=False)
    gen=out/'generated';gen.mkdir();prepare(root,gen)
    source=gen/'hle_thread_ohos.c';text=source.read_text(encoding='utf-8')
    begin=text.index('static int64_t block_inner(thr_t *t, int kind, int64_t ns) {')
    inner=text[begin:text.index('// ---------- 锁 ----------',begin)]
    begin=text.index('    case 0x18: {')
    case=text[begin:text.index('    case 0x19:',begin)]
    constants='\n'.join(re.findall(r'^#define R_\w+\s+0x[0-9A-Fa-f]+u',text,re.M))
    pre=r'''
#include <stdint.h>
#include <stddef.h>
#define ETIMEDOUT 110
#define CLOCK_REALTIME 0
typedef struct thr { int cancel,wkind,woke,cv; int64_t wres; uint64_t wseq; struct thr *next; } thr_t;
struct timespec { int64_t tv_sec,tv_nsec; };
static thr_t thread,*g_wait;
static uint64_t g_wseq,tick;
static int G,mode,failure,waits,signals,violations;
static thr_t *cur(void) {return &thread;}
static int xblr_execution_failed(void) {return failure;}
static int sw_detclock_enabled(void) {return mode==7;}
static uint64_t guest_ns(void) {return tick;}
static uint64_t now_mono_ns(void) {return tick;}
static int clock_gettime(int clock,struct timespec *out) {(void)clock;out->tv_sec=0;out->tv_nsec=0;return 0;}
static void pthread_mutex_lock(int *p) {if(*p)++violations;*p=1;}
static void pthread_mutex_unlock(int *p) {if(!*p)++violations;*p=0;}
static void wait_unlink(thr_t *t) {
 for(thr_t **p=&g_wait;*p;p=&(*p)->next)if(*p==t){*p=t->next;break;}t->next=NULL;
}
static int scheduled_wait(int *cv,int *lock) {
 (void)cv;if(!*lock)++violations;
 if(++waits>8)return ETIMEDOUT; /* bound the negative control's bad retry */
 if(mode==1){failure=1;return 0;}
 if(mode==3)return ETIMEDOUT;
 if(mode==7){tick+=2000;return 0;}
 if(mode==5&&waits==1)return 0; /* spurious wake */
 if(mode==4)thread.cancel=1;else signals=1;
 wait_unlink(&thread);thread.woke=1;thread.wkind=0;thread.wres=-1;return 0;
}
static int pthread_cond_wait(int *cv,int *lock) {return scheduled_wait(cv,lock);}
static int pthread_cond_timedwait(int *cv,int *lock,const struct timespec *dl) {(void)dl;return scheduled_wait(cv,lock);}
static int sig_obj(uint32_t handle,int *bad) {
 if(handle!=0x1010){*bad=1;return 0;}return signals;
}
'''
    post=r'''
static int64_t block(thr_t *t,int kind,int64_t ns) {return block_inner(t,kind,ns);}
static int dispatch(uint64_t *regs) {switch(0x18) {
'''+case+r'''
}return 0;}
uint64_t run_case(int m,int initial_cancel,int ready,int bad,int64_t ns,uint64_t *report) {
 thread=(thr_t){0};g_wait=NULL;g_wseq=0;tick=0;G=waits=violations=0;
 mode=m;failure=(m==0);signals=ready;thread.cancel=initial_cancel;
 uint32_t handle=bad?0x9999:0x1010;uint64_t regs[4]={0,(uintptr_t)&handle,1,(uint64_t)ns};
 dispatch(regs);report[0]=regs[1];report[1]=waits;report[2]=G;
 report[3]=g_wait!=NULL;report[4]=thread.wkind;report[5]=violations;report[6]=thread.cancel;
 return regs[0];
}
'''
    harness=constants+pre+inner+post
    (out/'wait_case.c').write_text(harness,encoding='utf-8')
    # Negative control removes only our new outer-loop propagation and retains
    # the same block_inner failure behavior that exposed the original hang.
    negative=harness.replace('            if (xblr_execution_failed()) { r = R_CANCELLED; break; }\n','')
    negative=negative.replace('if(br==R_TIMEOUT||br==R_CANCELLED)', 'if(br==R_TIMEOUT)')
    # The bug does not reach a second condition wait, so bound block calls too.
    negative=negative.replace('return block_inner(t,kind,ns);','if(++violations>8)return R_TIMEOUT;return block_inner(t,kind,ns);')
    (out/'wait_old.c').write_text(negative,encoding='utf-8')
    commands=[]
    for filename in ('wait_case','wait_old'):
        argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-O2','-shared','-nostdlib','-fuse-ld=lld',
              '-Wl,/noentry,/nodefaultlib,/export:run_case',str(out/(filename+'.c')),str(root/'tests/windows_gate_support.c'),'-o',str(out/(filename+'.dll'))]
        p=subprocess.run(argv,capture_output=True,text=True);commands.append(argv)
        (out/(filename+'.log')).write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
    (out/'commands.json').write_text(json.dumps(commands,indent=2))
    def load(name):
        dll=C.CDLL(str(out/name));fn=dll.run_case
        fn.argtypes=[C.c_int]*4+[C.c_int64,C.POINTER(C.c_uint64)];fn.restype=C.c_uint64
        return dll,fn
    live,run=load('wait_case.dll');rows=[]
    cases=[
      ('failure before wait',(0,0,0,0,-1),0xec01,0,0),
      ('failure on timed wait',(1,0,0,0,1000),0xec01,1,0),
      ('failure on indefinite wait',(1,0,0,0,-1),0xec01,1,0),
      ('ready object',(2,0,1,0,-1),0,0,0),
      ('normal event wake',(2,0,0,0,-1),0,1,0),
      ('timed timeout',(3,0,0,0,1000),0xea01,1,0),
      ('immediate timeout',(3,0,0,0,0),0xea01,0,0),
      ('cancel pending',(2,1,0,0,-1),0xec01,0,0),
      ('cancel during wait',(4,0,0,0,-1),0xec01,1,0),
      ('spurious then event',(5,0,0,0,-1),0,2,0),
      ('invalid handle',(2,0,0,1,-1),0xe401,0,0),
      ('guest clock timeout',(7,0,0,0,1000),0xea01,1,0),
    ]
    for name,args,result,waits,cancel in cases:
        report=(C.c_uint64*7)();actual=run(*args,report)
        assert actual==result and list(report)==[0,waits,0,0,0,0,cancel],(name,actual,list(report))
        rows.append({'name':name,'result':actual,'report':list(report)})
    old,oldrun=load('wait_old.dll');report=(C.c_uint64*7)()
    wrong=oldrun(1,0,0,0,-1,report)
    assert wrong==0xea01 and report[5]==9,(wrong,list(report))
    result={'result':'PASS','cases':len(rows),'negative_control':'old loop ignored failure and hit 9-call watchdog',
            'scope':'exact generated C wait bodies with deterministic scheduled pthread wake stand-ins; no device/thread-race proof',
            'generated_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'rows':rows}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:result[k] for k in ('result','cases','negative_control','scope')}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('native_sdk',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();main(a.native_sdk.resolve(),a.output.resolve())
