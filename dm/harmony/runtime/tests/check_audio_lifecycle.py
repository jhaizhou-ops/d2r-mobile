"""Compile production audio lifecycle helpers with instrumented device/thread APIs.

Checks ownership, locking, failures and focus sequences without opening audio.
This does not prove OHAudio callback timing or audible output on Harmony.
"""
import argparse,ctypes as C,hashlib,json,subprocess
from pathlib import Path

HARNESS=r'''
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
void *memset(void *,int,size_t);
typedef unsigned Result;
#define RES_OK 0u
#define RES_FAIL 0x2a2u
#define LOG(...) ((void)0)
enum { STATE_STARTED=0,STATE_STOPPED=1,AOMAX=8 };
typedef enum { AUDIOSTREAM_STATE_INVALID=-1,AUDIOSTREAM_STATE_PREPARED=1,AUDIOSTREAM_STATE_RUNNING=2,
 AUDIOSTREAM_STATE_STOPPED=3,AUDIOSTREAM_STATE_PAUSED=5 } OH_AudioStream_State;
struct aout;
typedef struct OH_AudioRenderer {struct aout *owner;int start_error,pause_error,query_error;
 OH_AudioStream_State state;int running,start_calls,pause_calls;} OH_AudioRenderer;
typedef struct aout { OH_AudioRenderer *au;int sink;_Atomic int state,null_run;unsigned null_th;
 int silent,creates,joins,index;} aout;
static aout outputs[AOMAX],*g_aos[AOMAX];
static OH_AudioRenderer devices[AOMAX];
static int g_aos_lock,g_suspended,violations,create_error,join_error;
static void pthread_mutex_lock(int *p){if(*p)++violations;*p=1;}
static void pthread_mutex_unlock(int *p){if(!*p)++violations;*p=0;}
static void *null_sink(void *p){return p;}
static int pthread_create(unsigned *th,const void *attr,void *(*fn)(void *),void *ctx) {
 (void)attr;(void)fn;aout *a=ctx;
 if(!g_aos_lock||a->silent||!a->null_run||(a->au&&a->au->running))++violations;
 ++a->creates;if(create_error)return create_error;
 *th=(unsigned)a->index+1;a->silent=1;return 0;
}
static int pthread_join(unsigned th,void *out){(void)out;
 if(!g_aos_lock||th<1||th>AOMAX){++violations;return 1;}aout *a=&outputs[th-1];
 if(a->null_run||!a->silent)++violations;++a->joins;if(join_error)return join_error;a->silent=0;return 0;
}
static int OH_AudioRenderer_Start(OH_AudioRenderer *d){
 if(!g_aos_lock||d->running||d->owner->silent)++violations;++d->start_calls;
 if(d->start_error)return d->start_error;d->state=AUDIOSTREAM_STATE_RUNNING;d->running=1;return 0;
}
static int OH_AudioRenderer_Pause(OH_AudioRenderer *d){
 if(!g_aos_lock)++violations;++d->pause_calls;if(d->pause_error)return d->pause_error;
 d->state=AUDIOSTREAM_STATE_PAUSED;d->running=0;return 0;
}
static int OH_AudioRenderer_GetCurrentState(OH_AudioRenderer *d,OH_AudioStream_State *s){
 if(!g_aos_lock)++violations;*s=d->state;return d->query_error;
}
#include "audio_ohos_lifecycle.inc"
void setup(int count,int real){
 memset(outputs,0,sizeof outputs);memset(devices,0,sizeof devices);memset(g_aos,0,sizeof g_aos);
 g_aos_lock=g_suspended=violations=create_error=join_error=0;
 for(int i=0;i<count;++i){aout *a=&outputs[i];a->index=i;a->state=STATE_STOPPED;
 devices[i].owner=a;devices[i].state=AUDIOSTREAM_STATE_PREPARED;a->au=real?&devices[i]:NULL;g_aos[i]=a;}
}
int start(int i){return xblr_audio_start(&outputs[i]);}
int stop(int i){return xblr_audio_stop(&outputs[i]);}
int focus(int on){return xblr_audio_suspend_all(on);}
void fault(int i,int start,int pause,int query,int state){OH_AudioRenderer *d=&devices[i];
 d->start_error=start;d->pause_error=pause;d->query_error=query;
 if(state>=0){d->state=(OH_AudioStream_State)state;d->running=state==AUDIOSTREAM_STATE_RUNNING;}
}
void thread_fault(int create,int join){create_error=create;join_error=join;}
void snapshot(int i,int *v){aout *a=&outputs[i];OH_AudioRenderer *d=&devices[i];
 int fields[]={a->sink,a->state,a->silent,d->running,a->creates,a->joins,d->start_calls,d->pause_calls,g_suspended,g_aos_lock,violations};
 for(unsigned j=0;j<sizeof fields/sizeof fields[0];++j)v[j]=fields[j];
}
'''

def main(sdk,out):
    root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
    src=out/'audio_case.c';src.write_text(HARNESS,encoding='utf-8')
    argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-O2','-shared','-nostdlib','-fuse-ld=lld',
          '-Wl,/noentry,/nodefaultlib,/export:setup,/export:start,/export:stop,/export:focus,/export:fault,/export:thread_fault,/export:snapshot',
          '-I',str(root/'src'),str(src),str(root/'tests/windows_gate_support.c'),'-o',str(out/'audio_case.dll')]
    p=subprocess.run(argv,capture_output=True,text=True);(out/'command.json').write_text(json.dumps(argv,indent=2))
    (out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
    dll=C.CDLL(str(out/'audio_case.dll'))
    dll.setup.argtypes=[C.c_int,C.c_int];dll.fault.argtypes=[C.c_int]*5
    dll.thread_fault.argtypes=[C.c_int]*2;dll.snapshot.argtypes=[C.c_int,C.POINTER(C.c_int)]
    rows=[]
    def check(name,index,sink,state,silent,real,counts=None):
        data=(C.c_int*11)();dll.snapshot(index,data);got=list(data)
        assert got[:4]==[sink,state,silent,real] and got[9:]==[0,0],(name,got)
        if counts is not None:assert got[4:8]==counts,(name,got)
        rows.append({'name':name,'output':index,'state':got})
    dll.setup(1,1);assert dll.start(0)==0;check('foreground starts renderer',0,1,0,0,1,[0,0,1,0])
    assert dll.start(0)==0;check('repeated start is idempotent',0,1,0,0,1,[0,0,1,0])
    assert dll.focus(1)==0;check('background transfers to one silent consumer',0,2,0,1,0,[1,0,1,1])
    assert dll.focus(1)==0;check('repeated background does not spawn again',0,2,0,1,0,[1,0,1,1])
    assert dll.focus(0)==0;check('foreground joins silent before device start',0,1,0,0,1,[1,1,2,1])
    assert dll.stop(0)==0;check('stop pauses actual renderer',0,0,1,0,0,[1,1,2,2])
    assert dll.stop(0)==0;check('repeated stop is idempotent',0,0,1,0,0,[1,1,2,2])
    dll.setup(1,1);assert dll.focus(1)==0 and dll.start(0)==0
    check('start while background creates silent only',0,2,0,1,0,[1,0,0,0])
    assert dll.stop(0)==0 and dll.focus(0)==0
    check('foreground never restarts stopped guest audio',0,0,1,0,0,[1,1,0,0])
    dll.setup(1,0);assert dll.start(0)==0 and dll.focus(1)==0 and dll.focus(0)==0
    check('explicit silent mode unchanged by focus',0,2,0,1,0,[1,0,0,0])
    assert dll.stop(0)==0;check('explicit silent stop joins consumer',0,0,1,0,0,[1,1,0,0])
    dll.setup(2,1);assert dll.start(0)==0 and dll.start(1)==0 and dll.focus(1)==0
    dll.fault(1,100,0,0,-1);assert dll.focus(0)==1
    check('partial resume successful output stays renderer',0,1,0,0,1)
    check('partial resume failed output retains explicit silent policy',1,2,0,1,0)
    dll.fault(1,0,0,0,-1);assert dll.focus(0)==0
    check('repeated foreground keeps successful output unchanged',0,1,0,0,1,[1,1,2,1])
    check('repeated foreground retries only failed output',1,1,0,0,1,[2,2,3,1])
    dll.fault(1,100,0,0,-1);assert dll.focus(1)==0 and dll.focus(0)==1
    assert dll.focus(1)==0
    check('background still pauses partially resumed output',0,2,0,1,0,[3,2,3,3])
    check('background does not duplicate failed output consumer',1,2,0,1,0,[4,3,4,2])
    dll.fault(1,0,0,0,-1);assert dll.focus(0)==0
    check('later foreground retries first output',0,1,0,0,1)
    check('later foreground retries failed output',1,1,0,0,1)
    dll.setup(1,1);dll.fault(0,100,0,0,-1);assert dll.start(0)==0x2a2
    check('initial device start failure is an actual Result failure',0,0,1,0,0)
    dll.fault(0,0,0,0,-1);assert dll.start(0)==0
    dll.fault(0,0,100,0,-1);assert dll.focus(1)==1
    check('failed pause never starts concurrent silent consumer',0,1,0,0,1,[0,0,2,1])
    dll.fault(0,0,100,0,5);assert dll.focus(1)==0
    check('platform already paused accepted only after state query',0,2,0,1,0)
    dll.setup(1,1);assert dll.start(0)==0
    dll.fault(0,0,100,100,5);assert dll.focus(1)==1
    check('failed query cannot certify renderer quiescence',0,1,0,0,0)
    dll.setup(1,0);dll.thread_fault(11,0);assert dll.start(0)==0x2a2
    check('silent pthread creation failure is not success',0,0,1,0,0)
    dll.setup(1,1);assert dll.start(0)==0;dll.thread_fault(11,0);assert dll.focus(1)==1
    check('background consumer failure marks output stopped',0,0,1,0,0)
    result={'result':'PASS','cases':len(rows),'scope':'compiled production lifecycle with instrumented API scheduling; no OHAudio device or audible-output proof',
            'source_sha256':hashlib.sha256((root/'src/audio_ohos_lifecycle.inc').read_bytes()).hexdigest(),'rows':rows}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:result[k] for k in ('result','cases','scope')}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('native_sdk',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();main(a.native_sdk.resolve(),a.output.resolve())
