"""Production snapshot with a synthetic registry and instrumented trylock.
Not a device concurrency, CPU profiling, or guest wait correctness test.
"""
import argparse, ctypes as C, json, subprocess
from pathlib import Path

HARNESS=r'''
#include "xblr_execution.h"
#include <stdatomic.h>
void *memset(void*,int,size_t);
typedef struct thr {
 uint64_t id,entry,waddr,wkey,wseq; uint32_t handle; int started,exited,wkind;
 _Atomic uint64_t nsvc,last_lr; _Atomic int last_svc; struct thr *all;
} thr_t;
static thr_t rows[65],*g_all;
static int G,violation;
static int pthread_mutex_trylock(int *m){if(*m)return 1;*m=1;return 0;}
static void pthread_mutex_unlock(int *m){if(!*m)violation++;*m=0;}
static uint64_t now_ns(void){return 123456789;}
#include "thread_snapshot_ohos.inc"
int test_run(int scenario){
 static xblr_thread_snapshot out;memset(&out,0x5a,sizeof out);G=0;violation=0;g_all=0;
 for(int i=0;i<65;i++){
  rows[i]=(thr_t){.id=81+i,.entry=0x1000+i,.handle=0x1001+i,.started=1,
   .wkind=i%5,.waddr=0xdead0000+i,.wkey=0xbeef0000+i,.wseq=100+i,.nsvc=200+i,
   .last_lr=0xab0000+i,.last_svc=0x18,.all=i<64?rows+i+1:0};
 }
 if(scenario==0)return xblr_get_thread_snapshot(0,sizeof out)==40&&G==0;
 if(scenario==1)return xblr_get_thread_snapshot(&out,sizeof out-1)==40&&out.count==0x5a5a5a5a&&G==0;
 if(scenario==2){G=1;return xblr_get_thread_snapshot(&out,sizeof out)==47&&out.count==0x5a5a5a5a&&G==1;}
 if(scenario==3)return !xblr_get_thread_snapshot(&out,sizeof out)&&out.count==0&&!out.truncated&&out.monotonic_ns==123456789&&G==0;
 g_all=rows;
 if(scenario==4)rows[63].all=0;
 if(xblr_get_thread_snapshot(&out,sizeof out)||out.count!=64||out.truncated!=(scenario!=4)||G||violation)return 0;
 for(int i=0;i<64;i++){
  xblr_thread_observation *r=out.threads+i;
  if(r->handle!=0x1001u+i||r->id!=81u+i||r->svc_count!=200u+i||r->last_lr!=0xab0000u+i||r->last_svc!=0x18)return 0;
  if(r->wait_address!=(i%5>=1&&i%5<=3?0xdead0000u+i:0)||r->wait_key!=(i%5==2?0xbeef0000u+i:0)||r->wait_sequence!=(i%5?100u+i:0))return 0;
 }
 return out.abi_version==1&&out.struct_bytes==sizeof out;
}
'''

def main(sdk,out):
    root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
    c=out/'snapshot.c';c.write_text(HARNESS,encoding='utf-8')
    dll=out/'snapshot.dll'
    argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:test_run','-I',str(root/'include'),'-I',str(root/'src'),str(c),str(root/'tests/windows_gate_support.c'),'-o',str(dll)]
    p=subprocess.run(argv,capture_output=True,text=True)
    (out/'build.log').write_text(p.stdout+p.stderr);(out/'commands.json').write_text(json.dumps([argv],indent=2))
    assert p.returncode==0,p.stderr
    lib=C.CDLL(str(dll));lib.test_run.argtypes=[C.c_int];lib.test_run.restype=C.c_int
    names=['null rejected','size mismatch preserves output','busy lock preserves output','empty registry','64 rows exact','65 rows bounded and marked truncated']
    for i,n in enumerate(names):assert lib.test_run(i)==1,n
    result={'result':'PASS','scope':__doc__,'cases':names}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();main(a.sdk,a.output)
