"""Production snapshot publication with injected guest memory and lock contention."""
import argparse,ctypes,json,subprocess
from pathlib import Path
from check_ui_touch import PREFIX
STUBS=r'''
static _Atomic uint64_t world_tick_game;
static int pthread_mutex_trylock(int*p){if(*p)return 1;*p=1;return 0;}
static int reads,fail_read;
static int scan_reads,repeated_reads,forced_reads;
int metric(int i){return i==0?scan_reads:i==1?repeated_reads:forced_reads;}
static int xblr_guest_read(void*out,uint64_t a,size_t n){reads++;if(fail_read)return 0;memcpy(out,(void*)(uintptr_t)a,n);return 1;}
static int xblr_guest_string(char*out,size_t n,uint64_t a){if(!a)return 0;for(size_t i=0;i<n;i++){if(!xblr_guest_read(out+i,a+i,1))return 0;if(!out[i])return 1;}return 0;}
'''
SUFFIX=r'''
int run(void){
 xblr_interaction_snapshot s;
 if(xblr_get_interaction_snapshot(0,sizeof s)!=40||xblr_get_interaction_snapshot(&s,sizeof s-1)!=40)return 1;
 if(xblr_get_interaction_snapshot(&s,sizeof s)||s.valid||s.abi_version!=1||reads)return 2;
 interaction_lock=1;if(xblr_get_interaction_snapshot(&s,sizeof s)!=47)return 3;interaction_lock=0;
 thread.is_main=0;xblr_publish_interaction();if(reads)return 4;thread.is_main=1;
 uint32_t w=1600,h=900;memcpy(memory+0x211da38,&w,4);memcpy(memory+0x211da3c,&h,4);frames=100;
 xblr_publish_interaction();int before=reads;if(xblr_get_interaction_snapshot(&s,sizeof s)||reads!=before||s.valid!=3||s.viewport_width!=w||s.present_frame!=100||s.generation!=1||s.ui_width!=1||s.ui_height!=1)return 5;
 if(s.capabilities!=7||s.enabled_targets||s.mode)return 6;
 before=reads;for(int i=0;i<100;i++)xblr_publish_interaction();if(reads!=before||interaction.generation!=1)return 16;
 frames++;fail_read=1;xblr_publish_interaction();xblr_get_interaction_snapshot(&s,sizeof s);if(s.valid||s.viewport_width||s.generation!=2)return 7;
 fail_read=0;static uint64_t hud[2],children[1];hud[1]=(uintptr_t)"HUDPanel";children[0]=(uintptr_t)hud;root[11]=(uintptr_t)children;root[12]=1;uint64_t rp=(uintptr_t)root;memcpy(memory+0x2f05458,&rp,8);
 frames++;xblr_publish_interaction();xblr_get_interaction_snapshot(&s,sizeof s);if(s.valid!=3)return 15;
 uint64_t loaded[3]={ui_hash("PanelManager"),ui_hash("UnloadPanel"),(uintptr_t)"LoadScreenPanel"};xblr_interaction_observe(loaded);
 xblr_publish_interaction();xblr_get_interaction_snapshot(&s,sizeof s);if(s.valid!=15||s.mode!=1||s.navigation||s.enabled_targets!=2047)return 9;
 uint64_t msg[3]={ui_hash("PanelManager"),ui_hash("OpenPanel"),(uintptr_t)"NpcDialogPanel"};xblr_interaction_observe(msg);
 xblr_publish_interaction();xblr_get_interaction_snapshot(&s,sizeof s);if(s.navigation!=3||s.enabled_targets!=1024)return 10;
 msg[2]=(uintptr_t)"YouDiedModal";xblr_interaction_observe(msg);xblr_publish_interaction();xblr_get_interaction_snapshot(&s,sizeof s);if(s.navigation!=2||s.enabled_targets)return 11;
 msg[1]=ui_hash("UnloadAll");xblr_interaction_observe(msg);msg[1]=ui_hash("OpenPanel");msg[2]=(uintptr_t)"LoadScreenPanel";xblr_interaction_observe(msg);xblr_publish_interaction();xblr_get_interaction_snapshot(&s,sizeof s);if(s.mode||s.navigation!=1||s.enabled_targets)return 12;
 msg[1]=ui_hash("UnloadPanel");xblr_interaction_observe(msg);xblr_publish_interaction();xblr_get_interaction_snapshot(&s,sizeof s);if(s.mode!=1||s.navigation)return 13;
 frames++;root[12]=257;xblr_publish_interaction();xblr_get_interaction_snapshot(&s,sizeof s);if(s.valid!=3||s.enabled_targets)return 14;
 root[12]=1;frames++;before=reads;xblr_publish_interaction();scan_reads=reads-before;
 before=reads;uint64_t generation=interaction.generation;for(int i=0;i<100;i++)xblr_publish_interaction();repeated_reads=reads-before;
 if(!scan_reads||repeated_reads||interaction.generation!=generation)return 17;
 before=reads;xblr_publish_interaction_force(1);forced_reads=reads-before;if(forced_reads!=scan_reads||interaction.generation!=generation+1)return 18;
 frames++;before=reads;xblr_publish_interaction();if(reads-before!=scan_reads)return 19;
 fail_read=1;xblr_publish_interaction_force(1);if(interaction.valid)return 20;fail_read=0;
 before=reads;xblr_publish_interaction();if(reads!=before||interaction.valid)return 21;
 frames++;xblr_publish_interaction();if(interaction.valid!=15)return 22;
 return violations?8:0;
}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 header=(root/'include/xblr_interaction.h').read_text();types=header[header.index('enum {'):header.index('/* Exactly')]
 from check_overlay_config import function
 hashing=function((root/'src/ui_touch_ohos.inc').read_text(),'static uint64_t ui_hash(')
 c=out/'test.c';c.write_text(PREFIX.replace('root[8]','root[16]')+types+STUBS+hashing+(root/'src/interaction_ohos.inc').read_text()+SUFFIX)
 dll=out/'test.dll';a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run,/export:metric',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 lib=ctypes.CDLL(str(dll));rc=lib.run();assert rc==0,rc
 import hashlib
 result={'result':'PASS','checks':24,'scope':__doc__,'production_sha256':hashlib.sha256((root/'src/interaction_ohos.inc').read_bytes()).hexdigest(),'one_frame_scan_guest_reads':lib.metric(0),'additional_100_same_frame_getters_guest_reads':lib.metric(1),'forced_pending_or_cancel_scan_guest_reads':lib.metric(2),'not_verified':'device duration or frame rate; controlled one-HUD fixture only'};(out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
