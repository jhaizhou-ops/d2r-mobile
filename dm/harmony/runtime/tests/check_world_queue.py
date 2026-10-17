"""Production world queue through picker/planner/action pump with mock guest and clocks."""
import argparse,ctypes,json,subprocess,hashlib
from pathlib import Path
from check_ui_touch import PREFIX
from check_world_go import STUBS,SETUP
EXTRA=r'''
#define CLOCK_MONOTONIC 1
struct timespec {long long tv_sec,tv_nsec;};
static uint64_t queue_time=1;static int time_failure,publication_block;
static int clock_gettime(int clock,struct timespec*t){t->tv_sec=queue_time/1000000000ull;t->tv_nsec=queue_time%1000000000ull;return time_failure?-1:0;}
static void xblr_publish_interaction_force(int force);
'''
SUFFIX=r'''
static void xblr_publish_interaction_force(int force){if(force!=1)violations++;if(publication_block)interaction.navigation=1;}
static void ready(void){
 setup(1,100,100,101,100);memset(&world_request,0,sizeof world_request);world_active_request=0;queue_time=1;time_failure=publication_block=0;atomic_store(&world_focus,1);atomic_store(&world_stick_active,0);
 interaction=(xblr_interaction_snapshot){.capabilities=23,.valid=15,.mode=1,.viewport_width=200,.viewport_height=200};
 p64(memory+0x2d665d8,1);p64(memory+0x2e2a698,123);p32(arena+14000+0x170,1);xblr_world_tick_observe((uintptr_t)arena+14000);frames+=1;
}
int run(void){uint64_t request=99;
 ready();interaction.capabilities=7;if(xblr_world_tap(.5f,.5f,&request)!=47||request!=99||world_request.pending)return 1;
 ready();interaction.valid=1;if(xblr_world_tap(.5f,.5f,&request)!=47||request!=99)return 2;
 ready();if(xblr_world_tap(-.1f,.5f,&request)!=40||request!=99||xblr_world_tap(.5f,.5f,0)!=40)return 3;
 ready();thread.is_main=0;if(xblr_world_tap(.5f,.5f,&request)||!world_request.pending||down||writes)return 4;xblr_world_pump();if(down||writes||!world_request.pending)return 5;thread.is_main=1;
 xblr_world_pump();if(down!=1||interaction.completed_request!=request||interaction.request_result!=2||world_request.pending||badgate)return 6;
 xblr_world_pump();if(down!=1||up)return 7;frames+=2;xblr_world_pump();if(up)return 8;frames+=1;xblr_world_pump();if(up!=1)return 9;
 ready();xblr_world_tap(.5f,.5f,&request);xblr_cancel_world();xblr_world_pump();if(down||interaction.completed_request!=request||interaction.request_result!=-2)return 10;
 ready();xblr_world_tap(.5f,.5f,&request);publication_block=1;xblr_world_pump();if(down||interaction.request_result!=-1)return 11;
 ready();xblr_world_tap(.5f,.5f,&request);time_failure=1;xblr_world_pump();if(down||interaction.request_result!=-1)return 12;
 ready();xblr_world_tap(.5f,.5f,&request);xblr_world_pump();uint64_t first=request;frames++;xblr_world_tap(.5f,.5f,&request);xblr_world_pump();if(request==first||down!=2||up!=1||interaction.completed_request!=request||badgate)return 13;
 ready();position(arena+4608,0,40,40);xblr_world_tap(.5f,.5f,&request);xblr_world_pump();if(!world_active_request||!moves||down)return 14;frames++;xblr_world_note_stick(1,0);xblr_world_pump();if(world_active_request||interaction.request_result!=-1||down)return 15;
 ready();xblr_world_tap(.9f,.9f,&request);xblr_world_pump();if(down||interaction.request_result||interaction.completed_request!=request)return 16;
 ready();xblr_world_tap(.5f,.5f,&request);p64(memory+0x2d665d8,0);xblr_world_pump();if(down||interaction.request_result!=-1)return 17;
 ready();xblr_world_tap(.5f,.5f,&request);xblr_world_pump();xblr_cancel_world();frames++;xblr_world_pump();if(up!=1||world_go.release_player||badgate)return 18;
 ready();world_set_focus(0);request=99;if(xblr_world_tap(.5f,.5f,&request)!=47||request!=99)return 19;world_set_focus(1);if(xblr_world_tap(.5f,.5f,&request))return 20;
 ready();xblr_world_tap(.5f,.5f,&request);xblr_world_pump();frames++;time_failure=1;xblr_world_pump();if(up!=1||world_go.release_player)return 21;
 ready();xblr_world_note_stick(1,0);request=99;if(xblr_world_tap(.5f,.5f,&request)!=47||request!=99)return 22;xblr_world_note_stick(0,0);if(xblr_world_tap(.5f,.5f,&request))return 23;
 return 0;
}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 header=(root/'include/xblr_interaction.h').read_text();types=header[header.index('enum {'):header.index('/* Exactly')]
 stubs=STUBS.replace('if(off==0x4ad870)', 'if(off==0x90d110){float*p=(void*)(uintptr_t)a[2];p[0]=100;p[1]=a[3]?100:100;return 1;}\n if(off==0x4ad870)')
 setup=SETUP[:SETUP.index('int run(void)')]
 decl='static pthread_mutex_t interaction_lock=PTHREAD_MUTEX_INITIALIZER;static xblr_interaction_snapshot interaction;\n'
 code=PREFIX[:PREFIX.index('static uint64_t xblr_invoke_guest')]+types+decl+EXTRA+stubs+'\n'.join((root/f'src/world_{n}_ohos.inc').read_text(encoding='utf-8') for n in ('tick','memory','geometry','target','plan','store','go','queue'))+setup+SUFFIX
 c=out/'test.c';c.write_text(code,encoding='utf-8');dll=out/'test.dll';a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 r={'result':'PASS','checks':23,'scope':__doc__,'production_sha256':hashlib.sha256((root/'src/world_queue_ohos.inc').read_bytes()).hexdigest(),'not_verified':'actual HID wiring, runtime native build, game tick gate, device actions; WORLD_TAP remains disabled'};(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
