"""Production unit/collision snapshots with malformed, overflowing, and failed memory reads."""
import argparse,ctypes,json,subprocess
from pathlib import Path
from check_ui_touch import PREFIX
STUBS=r'''
static int fail;
static int xblr_guest_read(void*out,uint64_t address,size_t n){if(fail||address<4096||address>UINT64_MAX-n)return 0;memcpy(out,(void*)(uintptr_t)address,n);return 1;}
'''
SUFFIX=r'''
int run(void){
 uint64_t unit[12]={0},path[5]={0};unit[7]=(uintptr_t)path;uint16_t *small=(void*)path;small[1]=120;small[3]=340;int x=-1,y=-1;
 if(!world_unit_xy((uintptr_t)unit,&x,&y)||x!=120||y!=340)return 1;
 unit[0]=4;int32_t *large=(void*)path;large[4]=12;large[5]=34;if(!world_unit_xy((uintptr_t)unit,&x,&y)||x!=12||y!=34)return 2;
 fail=1;x=y=-1;if(world_unit_xy((uintptr_t)unit,&x,&y)||x!=-1||y!=-1)return 3;fail=0;
 if(world_field(&x,UINT64_MAX-1,4,4))return 4;
 uint64_t room[20]={0},grid[6]={0};uint16_t cells[6]={1,2,3,4,5,6},v=0;room[7]=(uintptr_t)grid;grid[4]=(uintptr_t)cells;
 int32_t *b=(void*)room; b[32]=100;b[33]=200;b[34]=3;b[35]=2;((int32_t*)grid)[2]=3;
 if(!world_room_cell((uintptr_t)room,102,201,&v)||v!=6)return 5;
 if(world_room_cell((uintptr_t)room,103,201,&v)||world_room_cell((uintptr_t)room,99,200,&v))return 6;
 ((int32_t*)grid)[2]=2;if(world_room_cell((uintptr_t)room,100,200,&v))return 7;
 fail=1;if(world_room_cell((uintptr_t)room,100,200,&v))return 8;
 return 0;
}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 c=out/'test.c';c.write_text(PREFIX+STUBS+(root/'src/world_memory_ohos.inc').read_text()+SUFFIX);dll=out/'test.dll'
 a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 r={'result':'PASS','checks':8,'scope':__doc__,'not_implemented':'movement and guest action calls'};(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
