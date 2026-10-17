"""Generated production overlay lookup: canonical overlay, flat fallback, and error propagation."""
import argparse,ctypes,json,subprocess,sys
from pathlib import Path
from check_overlay_config import function
STUBS=r'''
#include <stddef.h>
#define F_OK 0
#define ENOENT 2
static int errno,mode,calls,wrong;
typedef struct {char dir[64],overlay[64];} mount_t;
static int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a-*b;}
static const char*xblr_romfs_rest(const char*r,const char*p){if(strcmp(r,"base")||strcmp(p,"data/file"))wrong++;return "file";}
static int root_path(const char*r,const char*p,char*out,size_t n){(void)n;calls++;out[0]=0;if(!strcmp(r,"overlay")){if(strcmp(p,"data/file"))wrong++;if(mode){errno=mode==1?2:mode==2?13:36;return 0;}}else if(strcmp(r,"base")||strcmp(p,"file"))wrong++;return 1;}
static int access(const char*p,int f){(void)p;(void)f;return 0;}
'''
SUFFIX=r'''
int run(void){mount_t m={"base","overlay"};char out[64];
 if(!rom_overlay_path(&m,"data/file",out,sizeof out)||calls!=1)return 1;
 mode=1;calls=0;if(!rom_overlay_path(&m,"data/file",out,sizeof out)||calls!=2)return 2;
 mode=2;calls=0;if(rom_overlay_path(&m,"data/file",out,sizeof out)||calls!=1||errno!=13)return 3;
 mode=3;calls=0;if(rom_overlay_path(&m,"data/file",out,sizeof out)||calls!=1||errno!=36)return 4;
 return wrong?5:0;}
'''
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False);sys.path.insert(0,str(root/'tools'))
 from prepare_services import prepare
 generated=out/'generated';generated.mkdir();prepare(root,generated)
 source=(generated/'hle_fs_ohos.c').read_text(encoding='utf-8');body=function(source,'static int rom_overlay_path(')
 assert 'entry_rest=ri==0?xblr_romfs_rest(m->dir,rest):rest' in source
 assert source.index('if (g_mnt[i].overlay[0] && !create)')<source.index('if(g_mnt[i].ro){rest=xblr_romfs_rest')
 c=out/'test.c';c.write_text(STUBS+body+SUFFIX);dll=out/'test.dll'
 a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,'-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 r={'result':'PASS','scope':__doc__,'checks':6};(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
