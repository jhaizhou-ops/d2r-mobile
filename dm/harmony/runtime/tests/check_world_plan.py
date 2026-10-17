"""Production bounded planner vs original sw_world planner on controlled collision rooms."""
import argparse,ctypes,json,subprocess,hashlib
from pathlib import Path
from check_ui_touch import PREFIX
STUBS=r'''
static uint8_t rooms[97][160],grids[97][48],player[96],path[64];static uint64_t adjacent[97][2];static uint16_t cells[192*192];
static uint64_t fail_at;static size_t fail_bytes;static unsigned reads;
static int abs(int x){return x<0?-x:x;}static uint64_t w_now_ns(void){return 0;}
static struct {int type,n,i,nearest,tx,ty;uint16_t node[24][2];} g_go;
static int within(uint64_t a,size_t n,void*p,size_t z){uint64_t b=(uintptr_t)p;return a>=b&&a<=UINT64_MAX-n&&a+n<=b+z;}
static int xblr_guest_read(void*out,uint64_t a,size_t n){
 reads++;if(a==fail_at&&(!fail_bytes||n==fail_bytes))return 0;
 if(!(within(a,n,rooms,sizeof rooms)||within(a,n,grids,sizeof grids)||within(a,n,adjacent,sizeof adjacent)||within(a,n,cells,sizeof cells)))return 0;
 memcpy(out,(void*)(uintptr_t)a,n);return 1;
}
static void p64(void*p,uint64_t n){memcpy(p,&n,8);}static void p32(void*p,uint32_t n){memcpy(p,&n,4);}
static void setup(void){
 memset(rooms,0,sizeof rooms);memset(grids,0,sizeof grids);memset(cells,0,sizeof cells);memset(adjacent,0,sizeof adjacent);memset(&g_go,0,sizeof g_go);fail_at=fail_bytes=reads=0;thread.is_main=1;
 for(int i=0;i<97;i++){p32(rooms[i]+0x88,192);p32(rooms[i]+0x8c,192);p64(rooms[i]+0x38,(uintptr_t)grids[i]);p32(grids[i]+8,192);p64(grids[i]+0x20,(uintptr_t)cells);}
 p64(player+0x38,(uintptr_t)path);p64(path+0x20,(uintptr_t)rooms[0]);
}
'''
SUFFIX=r'''
static int compare(int px,int py,int x,int y,unsigned type){
 ((uint16_t*)path)[1]=px;((uint16_t*)path)[3]=py;
 g_go.type=type;xblr_world_route route;int rc=world_plan((uintptr_t)rooms[0],px,py,x,y,type,&route);if(rc<0)return 1;
 w_plan(memory,(uintptr_t)player,x,y);
 if(route.n!=g_go.n||route.nearest!=g_go.nearest)return 2;
 for(int i=0;i<route.n;i++)if(route.node[i][0]!=g_go.node[i][0]||route.node[i][1]!=g_go.node[i][1])return 3;
 if(route.expanded>24000)return 4;return 0;
}
int run(void){
 setup();if(compare(40,40,100,40,1))return 1;
 setup();for(int y=0;y<100;y++)cells[y*192+70]=1;if(compare(40,40,100,40,1))return 2;
 setup();for(int y=0;y<192;y++)cells[y*192+70]=0x100;if(compare(40,40,100,40,1))return 3;
 setup();for(int y=0;y<192;y++)cells[y*192+70]=0x400;if(compare(40,40,100,40,2))return 4;
 setup();if(compare(40,40,41,41,1))return 5;
 unsigned seed=123;for(int k=0;k<20;k++){setup();for(int y=20;y<130;y++)for(int x=20;x<130;x++){seed=seed*1664525+1013904223;cells[y*192+x]=(seed>>28)==0?1:0;}if(compare(40,40,100,100,k%2?1:2))return 6;}
 xblr_world_route route,saved;memset(&route,0x55,sizeof route);saved=route;
 setup();p64(rooms[0],1);p32(rooms[0]+0x40,1);if(world_plan((uintptr_t)rooms[0],40,40,100,40,1,&route)!=-1||route.n!=saved.n)return 7;
 setup();p32(rooms[0]+0x40,1);if(world_plan((uintptr_t)rooms[0],40,40,100,40,1,&route)!=-1||route.n!=saved.n)return 8;
 setup();p64(rooms[0],(uintptr_t)adjacent[0]);adjacent[0][0]=(uintptr_t)rooms[0];p32(rooms[0]+0x40,1);if(compare(40,40,100,40,1)||world_planner.count!=1)return 9;
 setup();for(int i=0;i<96;i++){p64(rooms[i],(uintptr_t)adjacent[i]);adjacent[i][0]=(uintptr_t)rooms[i+1];p32(rooms[i]+0x40,1);}if(world_plan((uintptr_t)rooms[0],40,40,100,40,1,&route)!=-1||route.n!=saved.n)return 10;
 setup();fail_at=(uintptr_t)cells+((40*192+41)*2);if(world_plan((uintptr_t)rooms[0],40,40,100,40,1,&route)!=-1||route.n!=saved.n)return 11;
 setup();p32(grids[0]+8,191);if(world_plan((uintptr_t)rooms[0],40,40,100,40,1,&route)!=-1||route.n!=saved.n)return 12;
 setup();thread.is_main=0;if(world_plan((uintptr_t)rooms[0],40,40,100,40,1,&route)!=-1||reads)return 13;
 setup();if(world_plan((uintptr_t)rooms[0],-1,40,100,40,1,&route)!=-1||reads)return 14;
 setup();p32(rooms[0]+0x40,33);if(world_plan((uintptr_t)rooms[0],40,40,100,40,1,&route)!=-1)return 15;
 setup();p64(grids[0]+0x20,UINT64_MAX-1);if(world_plan((uintptr_t)rooms[0],40,40,100,40,1,&route)!=-1)return 16;
 setup();if(compare(40,40,191,191,1))return 17;
 return 0;
}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 original=(root.parent/'host/sw_world.c').read_text(encoding='utf-8');reference=original[original.index('static int wp_room_has'):original.index('// 物件要求玩家')]
 # WP constants and original globals precede the extracted functions.
 reference='#define W_NEAR 2\n#define WP_W 192\n#define WP_CAP 24000\n#define WP_SEG 30\nstatic uint64_t g_wp_room[96];static int g_wp_nroom;\n'+reference
 c=out/'test.c';c.write_text(PREFIX[:PREFIX.index('static uint64_t xblr_invoke_guest')]+STUBS+(root/'src/world_memory_ohos.inc').read_text()+(root/'src/world_plan_ohos.inc').read_text()+reference+SUFFIX)
 dll=out/'test.dll';a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 r={'result':'PASS','checks':17,'random_original_comparisons':20,'scope':__doc__,'original_planner_sha256':hashlib.sha256(reference.encode()).hexdigest(),'production_sha256':hashlib.sha256((root/'src/world_plan_ohos.inc').read_bytes()).hexdigest(),'not_verified':'game room concurrency, real path execution, guest commands, device'};(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
