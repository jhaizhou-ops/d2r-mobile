"""Production world-go lifecycle and target-slot writer with controlled guest commands."""
import argparse,ctypes,json,subprocess,hashlib
from pathlib import Path
from check_ui_touch import PREFIX
STUBS=r'''
#define XBLR_EXEC_HOST_FAILURE 61
static uint8_t arena[16384],manager[4096];static uint16_t cells[192*192];static uint64_t fail_at,native_target;static int writes,write_fail,restore_fail,short_write,down,up,moves,fatal,badgate;static int last_x,last_y;
struct iovec {void *iov_base;size_t iov_len;};
#define SYS_process_vm_writev 1
#define EINTR 4
static int errno;static int getpid(void){return 1;}
static int within(uint64_t a,size_t n,void*p,size_t z){uint64_t b=(uintptr_t)p;return a>=b&&a<=UINT64_MAX-n&&a+n<=b+z;}
static int xblr_guest_read(void*out,uint64_t a,size_t n){if(a==fail_at)return 0;if(!(within(a,n,memory,sizeof memory)||within(a,n,arena,sizeof arena)||within(a,n,manager,sizeof manager)||within(a,n,cells,sizeof cells)))return 0;memcpy(out,(void*)(uintptr_t)a,n);return 1;}
static long mock_write(int sys,int pid,struct iovec*l,int ln,struct iovec*r,int rn,int flags){writes++;if(sys!=1||pid!=1||ln!=1||rn!=1||flags||l->iov_len!=8||r->iov_len!=8)badgate++;if(write_fail||(restore_fail&&writes==2))return -1;if(short_write)return 7;memcpy(r->iov_base,l->iov_base,8);return 8;}
#define syscall mock_write
#define xblr_execution_stop(code) do {fatal++;return 0;}while(0)
static float fabsf(float x){return x<0?-x:x;}
static uint64_t mock_gate(void*fn,const uint64_t*a,uint32_t n){
 if(!thread.is_main)badgate++;uint64_t off=(uint8_t*)fn-memory,player=(uintptr_t)arena+4096;
 if(off==0x45f9d0){if(n!=1||a[0]!=3)badgate++;return player;}
 if(off==0x45f5f0){if(n!=4||a[1]!=(a[2]&127)||a[0]!=(uintptr_t)memory+0x2d603a0+a[3]*0x400)badgate++;uint64_t u;memcpy(&u,(void*)(uintptr_t)(a[0]+a[1]*8),8);return u;}
 if(off==0x2acecc){if(n)badgate++;return (uintptr_t)manager;}
 if(off==0x2f7a0c){if(n!=2||a[0]!=(uintptr_t)manager||a[1]!=3)badgate++;return native_target;}
 if(off==0x4ace20){if(n!=5||a[0]!=player||a[4])badgate++;uint64_t t;memcpy(&t,manager+0xd10,8);if(a[1]==7){down++;if(t!=(uintptr_t)arena&&t!=native_target)badgate++;}else if(a[1]==2){up++;if(t!=0xabc)badgate++;}else badgate++;return 1;}
 if(off==0x4ad870){if(n!=4||a[0]!=3||a[1]!=player)badgate++;moves++;last_x=a[2];last_y=a[3];return 0;}
 if(off==0x464a70){if(n!=5)badgate++;return 1;}
 if(off==0x2f7b7c){if(n!=3)badgate++;return *(uint32_t*)(uintptr_t)a[2]==2?2:5;}
 badgate++;return 0;
}
#define xblr_invoke_guest mock_gate
static void p64(void*p,uint64_t n){memcpy(p,&n,8);}static void p32(void*p,uint32_t n){memcpy(p,&n,4);}
'''
SETUP=r'''
static xblr_world_clock clock_value;
static void position(uint8_t*path,unsigned type,int x,int y){if(type==2||type==4||type==5){p32(path+16,x);p32(path+20,y);}else{((uint16_t*)path)[1]=x;((uint16_t*)path)[3]=y;}}
static xblr_world_target setup(unsigned type,int px,int py,int tx,int ty){
 memset(memory,0,sizeof memory);memset(arena,0,sizeof arena);memset(manager,0,sizeof manager);memset(cells,0,sizeof cells);memset(&world_go,0,sizeof world_go);memset(&world_tile_retry,0,sizeof world_tile_retry);
 writes=write_fail=restore_fail=short_write=down=up=moves=fatal=badgate=0;fail_at=native_target=0;thread.is_main=1;
 p32(memory+0x2d20e4c,3);p64(memory+0x2d603a0+type*0x400+9*8,(uintptr_t)arena);p32(arena,type);p32(arena+8,9);p32(arena+12,3);p64(arena+0x38,(uintptr_t)arena+512);position(arena+512,type,tx,ty);
 p64(arena+4096+0x38,(uintptr_t)arena+4608);position(arena+4608,0,px,py);p64(arena+4608+0x20,(uintptr_t)arena+6000);p64(arena+6000+0x38,(uintptr_t)arena+6200);p32(arena+6000+0x88,192);p32(arena+6000+0x8c,192);p32(arena+6200+8,192);p64(arena+6200+0x20,(uintptr_t)cells);
 p64(arena+6000+0x18,(uintptr_t)arena+6500);p64(arena+6500+0x90,777);p64(manager+0xd10,0xabc);
 clock_value=(xblr_world_clock){.now=1,.frame=0,.game=888,.tick=1,.tick_valid=1};return (xblr_world_target){type,9,(uintptr_t)arena};
}
static int begin(xblr_world_target t){return world_go_begin(g_mod,&t,&clock_value);}
static int pump(int cancel){return world_go_pump(g_mod,&clock_value,cancel);}
int run(void){
 xblr_world_target t=setup(1,100,100,101,100);if(!begin(t)||pump(0)!=1||down!=1||up||writes!=2||*(uint64_t*)(manager+0xd10)!=0xabc||badgate)return 1;
 clock_value.frame=2;if(pump(0)||up)return 2;clock_value.frame=3;if(pump(0)||up!=1||badgate)return 3;clock_value.frame=4;if(pump(0)||up!=1)return 4;
 t=setup(1,100,100,101,100);begin(t);pump(0);clock_value.frame=1;if(pump(1)!=-1||up!=1||world_go.on)return 5;
 t=setup(1,40,40,100,40);begin(t);clock_value.stick=1;if(pump(0)!=-1||moves||down||world_go.on)return 6;
 t=setup(1,40,40,100,40);begin(t);clock_value.now=15000000002ull;if(pump(0)!=-1||moves||down)return 7;
 t=setup(1,40,40,100,40);begin(t);clock_value.tick_valid=0;if(pump(0)!=-1||moves||down)return 8;
 t=setup(1,100,100,101,100);begin(t);pump(0);if(!begin(t)||up!=1||world_go.release_player||badgate)return 9;
 t=setup(1,100,100,101,100);begin(t);pump(0);clock_value.game=999;if(pump(0)!=-1||up||world_go.release_player)return 10;
 t=setup(1,100,100,101,100);begin(t);write_fail=1;if(pump(0)!=-1||down||*(uint64_t*)(manager+0xd10)!=0xabc||world_go.on)return 11;
 t=setup(1,100,100,101,100);begin(t);short_write=1;if(pump(0)!=-1||down||*(uint64_t*)(manager+0xd10)!=0xabc)return 12;
 t=setup(1,100,100,101,100);begin(t);restore_fail=1;if(pump(0)!=-1||fatal!=1||down!=1)return 13;
 t=setup(1,40,40,100,40);begin(t);if(pump(0)||moves!=1||down||badgate)return 14;clock_value.frame=19;if(pump(0)||moves!=1)return 15;clock_value.frame=20;if(pump(0)||moves!=2)return 16;
 t=setup(1,40,40,100,40);begin(t);pump(0);for(int i=1;i<=3;i++){clock_value.tick+=26;clock_value.frame+=20;if(pump(0)||!world_go.on)return 17;}clock_value.tick+=26;clock_value.frame+=20;if(pump(0)!=-1||world_go.on)return 18;
 t=setup(4,100,100,101,100);begin(t);p32(arena+12,0);if(pump(0)!=-1||down)return 19;
 t=setup(1,100,100,101,100);begin(t);fail_at=(uintptr_t)arena+0x38;if(pump(0)!=-1||down)return 20;
 t=setup(1,100,100,101,100);thread.is_main=0;if(begin(t)||writes||down)return 21;
 t=setup(5,100,100,101,100);begin(t);if(pump(0)!=1||down!=1||world_tile_retry.at!=45||badgate)return 22;clock_value.frame=3;pump(0);clock_value.frame=45;if(pump(0)!=1||down!=2||world_tile_retry.tries!=1||badgate)return 23;clock_value.frame=48;pump(0);clock_value.frame=90;if(pump(0)!=1||down!=3||world_tile_retry.tries!=2)return 24;clock_value.frame=93;pump(0);clock_value.frame=135;if(pump(0)||down!=3||badgate)return 25;
 t=setup(5,100,100,101,100);native_target=(uintptr_t)arena+1024;p32(arena+1024,2);p64(arena+1024+0x38,(uintptr_t)arena+1536);position(arena+1536,2,101,101);begin(t);if(pump(0)!=1||down!=1||world_tile_retry.at||badgate)return 26;
 t=setup(5,100,100,101,100);begin(t);pump(0);clock_value.frame=45;clock_value.stick=1;if(pump(0)!=-1||down!=1||world_tile_retry.at||up!=1)return 27;
 t=setup(2,100,100,101,100);arena[0x1bd]=1;p64(memory+0x2d64a18+16,(uintptr_t)arena+7000);p64(arena+7000+0x1550,1);p64(arena+7000+0x1548,(uintptr_t)arena+13000);arena[13000+0x15e]=16;p64(arena+512,(uintptr_t)arena+8000);begin(t);if(pump(0)||down||world_go.alt!=1||!world_go.alt_on)return 28;
 return 0;
}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False);c=out/'test.c';c.write_text(PREFIX[:PREFIX.index('static uint64_t xblr_invoke_guest')]+STUBS+'\n'.join((root/f'src/world_{n}_ohos.inc').read_text(encoding='utf-8') for n in ('memory','geometry','target','plan','store','go'))+SETUP,encoding='utf-8');dll=out/'test.dll'
 a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 r={'result':'PASS','checks':28,'scope':__doc__,'source_sha256':{n:hashlib.sha256((root/f'src/world_{n}_ohos.inc').read_bytes()).hexdigest() for n in ('go','store')},'not_verified':'real manager write permissions and guest actions, server observation gate, complete world queue integration, device'};(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
