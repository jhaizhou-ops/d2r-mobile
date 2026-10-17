"""Production world picker with controlled guest gate and safe memory faults; no game execution."""
import argparse,ctypes,json,subprocess,hashlib
from pathlib import Path
from check_ui_touch import PREFIX
STUBS=r'''
static uint8_t arena[8192];static uint64_t fail_at,unprojectable;static size_t fail_bytes;static int calls,badgate,reads,category_override,selectable_value;
static float fabsf(float x){return x<0?-x:x;}
static int xblr_guest_read(void*out,uint64_t address,size_t n){
 reads++;if(address>UINT64_MAX-n||(address==fail_at&&(!fail_bytes||n==fail_bytes)))return 0;
 uint64_t b=(uintptr_t)memory,z=(uintptr_t)arena;
 if(!((address>=b&&address+n<=b+sizeof memory)||(address>=z&&address+n<=z+sizeof arena)))return 0;
 memcpy(out,(void*)(uintptr_t)address,n);return 1;
}
static uint64_t mock_gate(void*fn,const uint64_t*a,uint32_t n){
 calls++;if(!thread.is_main)badgate++;
 uint64_t off=(uint8_t*)fn-memory,u=(uintptr_t)arena;
 if(off==0x45f9d0){if(n!=1||a[0]!=3)badgate++;return u+4096;}
 if(off==0x45f5f0){if(n!=4||a[0]!=(uintptr_t)memory+0x2d603a0+4*0x400||a[1]!=9||a[2]!=9||a[3]!=4)badgate++;return u;}
 if(off==0x464a70){if(n!=5||a[1]||a[2]||a[3]!=UINT64_MAX||a[4]!=UINT64_MAX)badgate++;return selectable_value;}
 if(off==0x2f7b7c){if(n!=3||a[0]!=0x165||a[1]!=u+4096)badgate++;if(category_override)return category_override;return *(uint32_t*)(uintptr_t)a[2]==2?2:5;}
 if(off==0x90d110){if(n!=4||a[0]!=123||a[3]>1)badgate++;if(a[1]==unprojectable)return 0;float*p=(void*)(uintptr_t)a[2];p[0]=100;p[1]=a[3]?80:100;return 1;}
 if(off==0x5b23f0){if(n!=6||a[0]!=u||a[4]!=(uintptr_t)memory+0x1d5a900||a[5])badgate++;return 0;}
 badgate++;return 0;
}
#define xblr_invoke_guest mock_gate
static void put64(void*p,uint64_t n){memcpy(p,&n,8);}static void put32(void*p,uint32_t n){memcpy(p,&n,4);}
static void reset(unsigned type){
 memset(memory,0,sizeof memory);memset(arena,0,sizeof arena);fail_at=unprojectable=fail_bytes=0;calls=badgate=reads=category_override=0;selectable_value=1;thread.is_main=1;
 put32(memory+0x2d20e4c,3);put64(memory+0x2e2a698,123);put32(arena,type);put32(arena+8,9);put32(arena+12,3);
 put64(memory+0x2d603a0+type*0x400,(uintptr_t)arena);
}
'''
SUFFIX=r'''
static int unchanged(xblr_world_target*t){return t->type==99&&t->id==88&&t->unit==77;}
static xblr_world_target sentinel(void){return (xblr_world_target){99,88,77};}
int run(void){
 xblr_world_target out;
 reset(1);out=sentinel();if(!world_pick(g_mod,100,90,10,&out)||out.type!=1||out.id!=9||out.unit!=(uintptr_t)arena||badgate)return 1;
 reset(1);out=sentinel();if(!world_pick(g_mod,500,500,10,&out)||out.unit||out.id||out.type)return 2;
 reset(1);put64(memory+0x2f0fee8,1);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out)||calls)return 3;
 reset(1);put64(memory+0x2f0fee0,1);put64(memory+0x2f0fee8,1);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 4;
 reset(1);put64(memory+0x2f0fee8,513);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 5;
 reset(1);put64(arena+0x158,(uintptr_t)arena);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out)||reads>14000||calls>17000)return 6;
 reset(1);put64(arena+0x158,1);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 7;
 reset(1);put32(arena,2);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 8;
 reset(2);fail_at=(uintptr_t)arena+0x10;out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 9;
 reset(2);unprojectable=(uintptr_t)arena;fail_at=(uintptr_t)arena+0x10;out=sentinel();if(!world_pick(g_mod,100,90,10,&out)||out.unit)return 10;
 reset(2);put64(arena+0x10,1);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 11;
 reset(2);put64(arena+0x10,(uintptr_t)arena+512);put64(arena+512,1);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 12;
 reset(1);fail_at=(uintptr_t)arena;fail_bytes=12;out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 13;
 reset(1);thread.is_main=0;out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out)||calls||reads)return 14;
 reset(1);if(world_pick(g_mod,100,90,10,0)||calls||reads)return 15;
 reset(4);put64(memory+0x2d603a0+4*0x400+9*8,(uintptr_t)arena);put64(memory+0x2f0fee0,(uintptr_t)arena+512);put64(memory+0x2f0fee8,1);put32(arena+512,90);put32(arena+516,80);put32(arena+520,20);put32(arena+524,20);put32(arena+528,9);out=sentinel();if(!world_pick(g_mod,100,90,10,&out)||out.type!=4||badgate)return 16;
 reset(1);selectable_value=0;out=sentinel();if(!world_pick(g_mod,100,90,10,&out)||out.unit||badgate)return 17;
 reset(1);category_override=2;out=sentinel();if(!world_pick(g_mod,100,90,10,&out)||out.unit||badgate)return 18;
 reset(2);put64(arena+0x10,(uintptr_t)arena+512);put64(arena+512,(uintptr_t)arena+1024);put64(arena+0x78,(uintptr_t)arena+2048);put64(arena+2048+0x30,1);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out))return 19;
 reset(2);put64(arena+0x10,(uintptr_t)arena+512);put64(arena+512,(uintptr_t)arena+1024);put64(arena+0x78,(uintptr_t)arena+2048);put64(arena+2048+0x30,1);put64(arena+2048+0x28,(uintptr_t)arena+3072);put32(arena+3072,3);put64(arena+3080,(uintptr_t)arena+3500);put64(arena+3500+0x18,(uintptr_t)arena+3600);put32(arena+3608,-20);put32(arena+3612,20);put32(arena+3616,-30);put32(arena+3620,0);out=sentinel();if(!world_pick(g_mod,100,90,10,&out)||out.type!=2||badgate)return 20;
 reset(4);put64(memory+0x2f0fee0,(uintptr_t)arena+512);put64(memory+0x2f0fee8,1);put32(arena+512,90);put32(arena+516,80);put32(arena+520,20);put32(arena+524,20);put32(arena+528,9);put32(arena+8,10);put64(arena+0x158,(uintptr_t)arena);put64(memory+0x2d603a0+4*0x400+9*8,(uintptr_t)arena);out=sentinel();if(world_pick(g_mod,100,90,10,&out)||!unchanged(&out)||calls!=1||reads>9000)return 21;
 return 0;
}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 prefix=PREFIX[:PREFIX.index('static uint64_t xblr_invoke_guest')]
 c=out/'test.c';c.write_text(prefix+STUBS+'\n'.join((root/f'src/world_{n}_ohos.inc').read_text() for n in ('memory','geometry','target'))+SUFFIX)
 dll=out/'test.dll';a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 controls=[]
 for name,old,new,expected in [('null-label','||labels[1]>512||(labels[1]&&!labels[0])','||labels[1]>512',3),('silent-memory-failure','if(candidate<0)return 0;','if(candidate<0)continue;',9),('early-output','xblr_world_target result={0};','*out=(xblr_world_target){0};xblr_world_target result={0};',13)]:
  text=c.read_text();assert old in text
  nc=out/(name+'.c');nc.write_text(text.replace(old,new));nd=out/(name+'.dll');na=[str(nc) if str(x)==str(c) else str(nd) if str(x)==str(dll) else str(x) for x in a]
  np=subprocess.run(na,capture_output=True,text=True);(out/(name+'-build.log')).write_text(np.stdout+np.stderr);assert np.returncode==0,np.stderr
  rc=ctypes.CDLL(str(nd)).run();assert rc==expected,(name,rc,expected);controls.append({'mutation':name,'rejected_at_check':rc,'expected':expected})
 (out/'negative-controls.json').write_text(json.dumps({'result':'PASS','controls':controls},indent=2))
 r={'result':'PASS','checks':21,'scope':__doc__,'not_verified':'actual guest invocation, game projection, device, navigation or interaction','source_sha256':{n:hashlib.sha256((root/f'src/world_{n}_ohos.inc').read_bytes()).hexdigest() for n in ('memory','geometry','target')}};(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
