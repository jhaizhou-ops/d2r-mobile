"""Exercise production SVC lock/unwind code with deterministic mutex tracking.

Compiles the actual generated dispatch wrapper and execution_stop body. The
single-thread schedule substitutes TLS/mutexes and captures lock state exactly
at the longjmp boundary with a returning test shim. No guest instruction, real
device syscall, platform TLS or native setjmp/longjmp is executed by this test.
"""
import argparse,ctypes as C,hashlib,json,subprocess,sys
from pathlib import Path

PREFIX=r'''
#include <stdint.h>
#include <stdatomic.h>
#define _Thread_local
#define _Noreturn
#define PTHREAD_MUTEX_INITIALIZER 0
#define fprintf(...) ((void)0)
#define abort() __builtin_trap()
#define longjmp(p,n) return record_unwind()
typedef int pthread_mutex_t;
typedef struct {unsigned status,stage_entered;uint64_t original_pc,svc_number,gates;} xblr_execution_report;
typedef struct {xblr_execution_report *report;int escape_armed;void *escape[5];} xblr_guest_thread;
static xblr_execution_report report,first_failure;
static xblr_guest_thread thread;
static pthread_mutex_t failure_lock;
static atomic_uint execution_failure;
static int violations,locked,unlocked,wakes,memory_calls,thread_calls,mode,escaped,unwind_lock,unwind_owned;
static void pthread_mutex_lock(int *m){if(*m)++violations;*m=1;++locked;}
static void pthread_mutex_unlock(int *m){if(!*m)++violations;*m=0;++unlocked;}
static xblr_guest_thread *xblr_current_thread(void){return &thread;}
static void xblr_threads_wake_failure(void);
static void record_unwind(void);
static uint64_t memory_dispatch(int,uint64_t *,uint64_t);
static int svc_thread(int n,uint64_t *regs){++thread_calls;if(n==0x18){regs[0]=0x123;return 1;}return 0;}
'''
SUFFIX=r'''
static void xblr_threads_wake_failure(void){++wakes;if(memory_dispatch_lock||memory_dispatch_owned)++violations;}
static void record_unwind(void){escaped=1;unwind_lock=memory_dispatch_lock;unwind_owned=memory_dispatch_owned;}
static uint64_t memory_dispatch(int n,uint64_t *regs,uint64_t pc){
 (void)n;(void)pc;++memory_calls;if(!memory_dispatch_lock||!memory_dispatch_owned)++violations;
 if(mode==1){xblr_execution_stop(43);return 0;}
 if(mode==2){xblr_execution_stop(44);return 0;}
 regs[0]=0x456;return regs[0];
}
int run_case(int test,uint64_t *out){
 violations=locked=unlocked=wakes=memory_calls=thread_calls=escaped=unwind_lock=unwind_owned=0;failure_lock=memory_dispatch_lock=memory_dispatch_owned=0;
 execution_failure=0;report=(xblr_execution_report){0};first_failure=report;thread.report=&report;thread.escape_armed=1;mode=test;
 uint64_t regs[3]={0};
 if(test==3)xblr_execution_stop(49);else svc_dispatch(test==4?0x18:6,regs,0x123400);
 out[0]=escaped?unwind_lock:memory_dispatch_lock;out[1]=escaped?unwind_owned:memory_dispatch_owned;out[2]=failure_lock;
 out[3]=violations;out[4]=report.status;out[5]=execution_failure;
 out[6]=wakes;out[7]=memory_calls;out[8]=thread_calls;out[9]=regs[0];out[10]=locked;out[11]=unlocked;
 /* Also check the ordinary acquire/release path remains usable after the
  * returning test shim. The captured boundary above is the unwind evidence. */
 mode=0;svc_dispatch(6,regs,0x123404);out[12]=violations;out[13]=memory_dispatch_lock;return escaped;
}
'''

def main(sdk,out):
    root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'tools'))
    from prepare_services import prepare
    out.mkdir(parents=True,exist_ok=False);gen=out/'generated';gen.mkdir();prepare(root,gen)
    service=gen/'hle_svc_ohos.c';text=service.read_text(encoding='utf-8')
    wrapper=text[text.index('static pthread_mutex_t memory_dispatch_lock='):]
    source=root/'src/execution.c';text=source.read_text(encoding='utf-8')
    stop=text[text.index('_Noreturn void xblr_execution_stop('):text.index('static xblr_guest_thread *new_thread_context(')]
    assert stop.count('    xblr_svc_abort_dispatch();')==1
    commands=[];dlls=[]
    for name,body in [('svc_stop',stop),('svc_old',stop.replace('    xblr_svc_abort_dispatch();\n',''))]:
        c=out/(name+'.c');c.write_text(PREFIX+wrapper+body+SUFFIX,encoding='utf-8')
        argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-O2','-shared','-nostdlib','-fuse-ld=lld',
              '-Wl,/noentry,/nodefaultlib,/export:run_case',str(c),str(root/'tests/windows_gate_support.c'),'-o',str(out/(name+'.dll'))]
        p=subprocess.run(argv,capture_output=True,text=True);commands.append(argv)
        (out/(name+'.log')).write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
        dll=C.CDLL(str(out/(name+'.dll')));dll.run_case.argtypes=[C.c_int,C.POINTER(C.c_uint64)];dlls.append(dll)
    (out/'commands.json').write_text(json.dumps(commands,indent=2));rows=[]
    for mode,name,status,escape,memory,threads in [(0,'normal memory SVC',0,0,1,1),(1,'unsupported memory SVC',43,1,1,1),
        (2,'guest Break SVC',44,1,1,1),(3,'fatal outside SVC lock',49,1,0,0),(4,'thread SVC bypass',0,0,0,1)]:
        report=(C.c_uint64*14)();escaped=dlls[0].run_case(mode,report);got=list(report)
        assert escaped==escape and got[:4]==[0,0,0,0] and got[4:9]==[status,status,escape,memory,threads],(name,got,escaped)
        assert got[10]==got[11] and got[12:]==[0,0],(name,got)
        rows.append({'name':name,'escaped':escaped,'state':got})
    old=(C.c_uint64*14)();escaped=dlls[1].run_case(1,old)
    assert escaped==1 and list(old[:2])==[1,1] and old[12]>0
    result={'result':'PASS','cases':len(rows),'scope':'compiled production wrapper/stop; instrumented single-thread mutex schedule and returning longjmp-boundary shim, not native unwind/TLS verification',
        'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'generated_svc_sha256':hashlib.sha256(service.read_bytes()).hexdigest(),
        'rows':rows,'negative_control':{'escaped':escaped,'state':list(old),'failure':'old stop still owns the ledger mutex at the longjmp boundary'}}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:result[k] for k in ('result','cases','scope','negative_control')}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('native_sdk',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();main(a.native_sdk.resolve(),a.output.resolve())
