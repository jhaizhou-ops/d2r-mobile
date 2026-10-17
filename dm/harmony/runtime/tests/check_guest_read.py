"""Production fail-closed guest reader with injected syscall results, no guest execution."""
import argparse,ctypes,json,subprocess
from pathlib import Path
PREFIX=r'''
#include <stdint.h>
#include <stddef.h>
void *memcpy(void*,const void*,size_t);void *memset(void*,int,size_t);
struct iovec {void *iov_base;size_t iov_len;};
#define SYS_process_vm_readv 1
#define EINTR 4
static int errno,mode,calls;
static int getpid(void){return 1;}
static long syscall(int nr,int pid,struct iovec*l,int nl,struct iovec*r,int n,int flags){
 (void)nr;(void)pid;(void)nl;(void)n;(void)flags;++calls;
 if(mode==1)return -1;
 if(mode==2){memcpy(l->iov_base,r->iov_base,l->iov_len-1);return l->iov_len-1;}
 if(mode==3&&calls==1){errno=EINTR;return -1;}
 errno=0;memcpy(l->iov_base,r->iov_base,l->iov_len);return l->iov_len;
}
'''
SUFFIX=r'''
int run(void){
 char src[8]="hello",out[8];
 if(xblr_guest_read(out,0,8)||calls)return 1;
 if(xblr_guest_read(out,UINT64_MAX-2,8)||calls)return 2;
 if(xblr_guest_read(out,(uintptr_t)src,4097)||calls)return 3;
 if(!xblr_guest_read(out,(uintptr_t)src,8)||out[4]!='o')return 4;
 mode=1;errno=0;if(xblr_guest_read(out,(uintptr_t)src,8)||out[0])return 5;
 mode=2;if(xblr_guest_read(out,(uintptr_t)src,8)||out[0]||out[6])return 6;
 mode=3;calls=0;if(!xblr_guest_read(out,(uintptr_t)src,8)||calls!=2)return 7;
 mode=0;if(!xblr_guest_string(out,8,(uintptr_t)src)||out[4]!='o')return 8;
 if(xblr_guest_string(out,4,(uintptr_t)src)||out[0])return 9;
 return 0;
}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 c=out/'test.c';c.write_text(PREFIX+(root/'src/guest_read_ohos.inc').read_text()+SUFFIX)
 dll=out/'test.dll';a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 result={'result':'PASS','checks':9,'scope':__doc__,'device_process_vm_readv':'NOT_YET_VERIFIED'}
 (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
