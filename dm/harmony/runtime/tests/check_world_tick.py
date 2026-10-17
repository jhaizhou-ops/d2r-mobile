"""Production server-tick snapshot and exact r32 transparent observation-site guard."""
import argparse,ctypes,json,subprocess,sys,hashlib
from pathlib import Path
from check_ui_touch import PREFIX
STUBS=r'''
static uint8_t game[512];static uint64_t fail_at;
static int xblr_guest_read(void*out,uint64_t a,size_t n){if(a==fail_at)return 0;uint64_t b=(uintptr_t)memory,g=(uintptr_t)game;if(a>UINT64_MAX-n||!((a>=b&&a+n<=b+sizeof memory)||(a>=g&&a+n<=g+sizeof game)))return 0;memcpy(out,(void*)(uintptr_t)a,n);return 1;}
static void p64(void*p,uint64_t n){memcpy(p,&n,8);}static void p32(void*p,uint32_t n){memcpy(p,&n,4);}
'''
SUFFIX=r'''
int run(void){uint32_t tick=99;uint64_t identity=0;
 if(world_tick_read(g_mod,&identity,&tick)||tick!=99)return 1;
 xblr_world_tick_observe((uintptr_t)game);p64(memory+0x2d665d8,1);p32(game+0x170,123);
 if(!world_tick_read(g_mod,&identity,&tick)||tick!=123)return 2;
 p32(game+0x170,124);if(!world_tick_read(g_mod,&identity,&tick)||tick!=124)return 3;
 tick=99;fail_at=(uintptr_t)memory+0x2d665d8+1024;if(world_tick_read(g_mod,&identity,&tick)||tick!=99)return 4;
 fail_at=0;p64(memory+0x2d665d8,UINT64_MAX);if(world_tick_read(g_mod,&identity,&tick)||atomic_load(&world_tick_game))return 5;
 p64(memory+0x2d665d8,1);xblr_world_tick_observe(1);if(world_tick_read(g_mod,&identity,&tick)||tick!=99)return 6;
 xblr_world_tick_observe(UINT64_MAX);if(world_tick_read(g_mod,&identity,&tick)||tick!=99)return 7;
 return 0;}
'''
def main(sdk,originals,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False);c=out/'test.c';c.write_text(PREFIX[:PREFIX.index('static uint64_t xblr_invoke_guest')]+STUBS+(root/'src/world_tick_ohos.inc').read_text()+SUFFIX);dll=out/'test.dll'
 a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 sys.path.insert(0,str(root/'tools'));from rewrite_harmony import world_tick_observation_site,op_body,dis
 raw=bytearray((originals/'main.original.image.bin').read_bytes());site=world_tick_observation_site('main',raw);assert site[0]==0x5d3bc4 and site[3]==0x5754 and world_tick_observation_site('sdk',raw) is None
 ins=dis(site[1],site[0]);assert ins.mnemonic=='str' and ins.op_str=='d8, [sp, #0xd0]'
 (out/'observation-leaf.S').write_text('\n'.join(op_body(ins,'test_world_tick_observation')))
 raw[site[0]]^=1
 try:world_tick_observation_site('main',raw)
 except AssertionError:pass
 else:raise AssertionError('changed original tick opcode accepted')
 r={'result':'PASS','checks':9,'scope':__doc__,'site':'main+5d3bc4','original8':'ff0305d1e86b00fd','game_argument':'x0 unchanged since entry','not_verified':'full generated ARM64 gate and device tick lifecycle','production_sha256':hashlib.sha256((root/'src/world_tick_ohos.inc').read_bytes()).hexdigest()};(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('originals',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.originals,a.output)
