"""Production prefix transaction with injected protection failures and original NSO byte guards."""
import argparse,ctypes,json,subprocess,shutil
from pathlib import Path
from check_ui_touch import PREFIX
STUBS=r'''
#define XBLR_EXEC_HOST_FAILURE 49
#define PROT_READ 1
#define PROT_WRITE 2
#define _SC_PAGESIZE 30
static int calls,restores,fail_at,enabled=1;
static char*getenv(const char*n){(void)n;return enabled?"overlay":0;}
static long sysconf(int n){(void)n;return 4096;}
static size_t strlen(const char*s){size_t n=0;while(s[n])n++;return n;}
static int xblr_guest_read(void*out,uint64_t a,size_t n){memcpy(out,(void*)(uintptr_t)a,n);return 1;}
static int mprotect(void*p,size_t n,int prot){(void)p;if(n!=4096)violations++;if(prot==3){calls++;return calls==fail_at?-1:0;}if(prot!=1)violations++;restores++;return 0;}
'''
SUFFIX=r'''
int run(int mode){
 for(unsigned i=0;i<7;i++)memcpy(memory+padmode_prefixes[i].off,padmode_prefixes[i].pad,strlen(padmode_prefixes[i].pad)+1);
 if(mode==1)memory[padmode_prefixes[6].off]^=1;
 if(mode==2)fail_at=3;
 if(mode==3)enabled=0;
 int rc=xblr_padmode_boot();
 if(mode==1)return rc!=49||calls?1:0;
 if(mode==2){if(rc!=49||calls!=3||restores!=2)return 2;for(unsigned i=0;i<7;i++)if(strcmp((char*)memory+padmode_prefixes[i].off,padmode_prefixes[i].pad))return 3;return 0;}
 if(mode==3)return rc||calls?4:0;
 if(rc||!calls||calls!=restores||violations)return 5;
 for(unsigned i=0;i<7;i++)if(strcmp((char*)memory+padmode_prefixes[i].off,padmode_prefixes[i].touch))return 6;
 return 0;
}
'''
def main(sdk,out,originals):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 body=(root/'src/padmode_boot_ohos.inc').read_text();c=out/'test.c';c.write_text(PREFIX+STUBS+body+SUFFIX)
 dll=out/'test.dll';a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 for i in range(4):
  copy=out/f'case{i}.dll';shutil.copy2(dll,copy);rc=ctypes.CDLL(str(copy)).run(i);assert rc==0,(i,rc)
 import re
 raw=(originals/'main.original.image.bin').read_bytes();guards=[]
 for off,pad,touch in re.findall(r'\{(0x[0-9a-f]+),"([^"]+)","([^"]+)"\}',body):
  o=int(off,16);assert len(pad)==len(touch);assert raw[o:o+len(pad)+1]==pad.encode()+b'\0';guards.append({'offset':off,'original':pad,'replacement':touch})
 assert len(guards)==7
 result={'result':'PASS','scope':__doc__,'cases':['all seven writes and RO restore','last guard mismatch performs no writes/protection changes','third protection failure restores first two with no writes','no overlay no writes'],'guards':guards}
 (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);p.add_argument('originals',type=Path);a=p.parse_args();main(a.sdk,a.output,a.originals)
