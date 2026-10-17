"""Production overlay configuration with controlled filesystem failures; no real assets written."""
import argparse,ctypes,json,subprocess
from pathlib import Path
STUBS=r'''
#include <stddef.h>
#include <stdarg.h>
#define PATH_MAX 4096
#define XBLR_EXEC_ARGUMENT 40
#define XBLR_EXEC_BUSY 47
#define XBLR_EXEC_HOST_FAILURE 49
#define R_OK 4
#define S_ISREG(m) ((m)==1)
void*memcpy(void*,const void*,size_t);
static size_t strlen(const char*s){size_t n=0;while(s[n])n++;return n;}
static int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *(unsigned char*)a-*(unsigned char*)b;}
static int strncmp(const char*a,const char*b,size_t n){while(n--){if(*a!=*b)return *a-*b;if(!*a)return 0;a++;b++;}return 0;}
static char*strchr(const char*s,int c){do{if(*s==c)return (char*)s;}while(*s++);return 0;}
static int snprintf(char*out,size_t cap,const char*f,...){va_list a;va_start(a,f);size_t n=0;while(*f){if(*f=='%'&&f[1]=='s'){const char*s=va_arg(a,const char*);while(*s){if(n+1<cap)out[n]=*s;n++;s++;}f+=2;}else{if(n+1<cap)out[n]=*f;n++;f++;}}if(cap)out[n<cap?n:cap-1]=0;va_end(a);return (int)n;}
static char configured_romfs[4096]="/rom",setting[4096]="/old";
static int mode,started,entry_index,changes;
static int xblr_execution_has_started(void){return started;}
static char*getenv(const char*n){return !strcmp(n,"SW_SAVE")?"/save":!strcmp(n,"SW_TEMP")?"/tmp":setting;}
static int setenv(const char*k,const char*v,int replace){(void)k;(void)replace;if(mode==5)return -1;memcpy(setting,v,strlen(v)+1);changes++;return 0;}
static int unsetenv(const char*k){(void)k;setting[0]=0;changes++;return 0;}
static char*realpath(const char*p,char*out){if(mode==2&&!strcmp(p,"/overlay/data/.build.config")){memcpy(out,"/outside/file",14);return out;}memcpy(out,p,strlen(p)+1);return out;}
struct stat{int st_mode;long long st_size;};
static int stat(const char*p,struct stat*s){if(mode==1&&!strcmp(p,"/overlay/data/.build.config"))return -1;s->st_mode=1;s->st_size=100;return 0;}
static int access(const char*p,int m){(void)p;(void)m;return mode==6?-1:0;}
typedef int DIR;struct dirent{char d_name[64];};
static DIR*opendir(const char*p){(void)p;entry_index=0;return (DIR*)1;}
static struct dirent*readdir(DIR*d){(void)d;static struct dirent e={"00-00000001.data"};return entry_index++<(mode==3?0:mode==4?2:1)?&e:0;}
static int closedir(DIR*d){(void)d;return 0;}
'''
SUFFIX=r'''
int run(void){
 if(xblr_configure_game_overlay("/overlay")||strcmp(setting,"/overlay"))return 1;
 for(mode=1;mode<=6;mode++){int before=changes;int r=xblr_configure_game_overlay("/overlay");if(r!=(mode==5?49:40)||changes!=before||strcmp(setting,"/overlay"))return 10+mode;}
 mode=0;const char*bad[]={"relative","/rom/child","/save","/tmp/child","/save.work/child","/overlay:other"};
 for(unsigned i=0;i<6;i++)if(xblr_configure_game_overlay(bad[i])!=40)return 20+i;
 started=1;if(xblr_configure_game_overlay(0)!=47||strcmp(setting,"/overlay"))return 30;started=0;
 if(xblr_configure_game_overlay(0)||setting[0])return 31;
 return 0;
}
'''
def function(source,name):
 start=source.index(name);brace=source.index('{',start);end=brace+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
def main(sdk,out):
 root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
 source=(root/'src/services_ohos.c').read_text();c=out/'test.c';c.write_text(STUBS+function(source,'int xblr_fs_roots_disjoint(')+function(source,'int xblr_configure_game_overlay(')+SUFFIX)
 dll=out/'test.dll';a=[sdk/'llvm/bin/clang.exe','--target=x86_64-pc-windows-msvc','-O2','-mno-stack-arg-probe','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run',c,root/'tests/windows_gate_support.c','-o',dll]
 p=subprocess.run(list(map(str,a)),capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 rc=ctypes.CDLL(str(dll)).run();assert rc==0,rc
 result={'result':'PASS','scope':__doc__,'checks':['valid physical CASC package','missing config','symlink escape','zero archives','multiple archives','setenv error','read permission failure','six invalid/overlap roots','started preserves old','NULL clears old'],'not_tested':'CASC logical index/hash integrity belongs to importer'}
 (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
