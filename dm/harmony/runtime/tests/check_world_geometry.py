"""Original world-target geometry priority and no-empty-ground selection, production pure C helper."""
import argparse,ctypes,json,subprocess
from pathlib import Path
from check_ui_touch import PREFIX
SUFFIX=r'''
int run(void){
 xblr_world_hit hit={0};xblr_world_candidate npc={.identity=1,.cx=100,.cy=100,.width=44,.height=100};
 world_hit_consider(&hit,&npc,151,100,90);if(hit.identity)return 1;
 world_hit_consider(&hit,&npc,145,100,90);if(hit.identity!=1||hit.rank)return 2;
 xblr_world_candidate door={.identity=2,.cx=0,.cy=0,.width=1,.height=1,.selection_x=140,.selection_y=100,.selection_width=90,.selection_height=90};
 world_hit_consider(&hit,&door,145,100,90);if(hit.identity!=1)return 3;
 hit=(xblr_world_hit){0};world_hit_consider(&hit,&door,145,100,90);if(hit.identity!=2||hit.rank!=1)return 4;
 world_hit_consider(&hit,&npc,145,100,90);if(hit.identity!=1||hit.rank)return 5;
 xblr_world_candidate waypoint={.identity=3,.cx=0,.cy=0,.width=1,.height=1,.foot_x=100,.foot_y=100,.diamond_half_width=100,.diamond_half_height=50};
 hit=(xblr_world_hit){0};world_hit_consider(&hit,&waypoint,175,140,10);if(hit.identity)return 6;
 world_hit_consider(&hit,&waypoint,150,125,10);if(hit.identity!=3)return 7;
 xblr_world_candidate closer=npc;closer.identity=4;closer.cx=144;
 hit=(xblr_world_hit){0};world_hit_consider(&hit,&npc,145,100,90);world_hit_consider(&hit,&closer,145,100,90);if(hit.identity!=4)return 8;
 hit=(xblr_world_hit){0};world_hit_consider(&hit,&npc,100,100,-1);if(hit.identity)return 9;
 return 0;
}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 c=out/'test.c';c.write_text(PREFIX+(root/'src/world_geometry_ohos.inc').read_text()+SUFFIX);dll=out/'test.dll'
 a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 r={'result':'PASS','checks':9,'scope':__doc__,'not_implemented':'guest target snapshot and movement adapter; WORLD_TAP capability remains off'}
 (out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
