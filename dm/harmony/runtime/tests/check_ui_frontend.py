"""Production front-end routing with controlled UI messages and save directory; not game execution."""
import argparse,ctypes as C,json,shutil,subprocess,sys
from pathlib import Path
from check_ui_touch import PREFIX
STUBS=r'''
static void xblr_publish_interaction(void){}
static void xblr_interaction_observe(const uint64_t*m){(void)m;}
struct timespec{long long tv_sec,tv_nsec;};
#define CLOCK_MONOTONIC 1
static int clock_gettime(int c,struct timespec*t){(void)c;t->tv_sec=0;t->tv_nsec=0;return 0;}
static int xblr_guest_read(void*out,uint64_t address,size_t n){if(address<4096)return 0;memcpy(out,(void*)(uintptr_t)address,n);return 1;}
static int xblr_guest_string(char*out,size_t n,uint64_t a){for(size_t i=0;i<n;i++){if(!xblr_guest_read(out+i,a+i,1))return 0;if(!out[i])return 1;}return 0;}
static int scenario,seen_file;
static size_t strlen(const char*s){size_t n=0;while(s[n])n++;return n;}
static char*getenv(const char*n){
 if(!strcmp(n,"SW_SAVE"))return "save";
 if(!strcmp(n,"SW_KEYS")&&(scenario==4||scenario==6))return "script";
 if(!strcmp(n,"SW_FE_AUTO"))return scenario==5?"0":scenario==6?"1":0;
 return 0;
}
static int atoi(const char*s){return *s=='1';}
typedef int DIR;struct dirent{char d_name[32];};struct stat{int st_mode;};
#define S_ISDIR(x) 0
static DIR *opendir(const char*p){(void)p;seen_file=0;return (DIR*)1;}
static struct dirent*readdir(DIR*d){(void)d;static struct dirent e={"saved.d2s"};return scenario==1&&!seen_file++?&e:0;}
static int closedir(DIR*d){(void)d;return 0;}
static int stat(const char*p,struct stat*s){(void)p;(void)s;return -1;}
static int snprintf(char*p,size_t n,const char*f,...){(void)n;(void)f;*p=0;return 0;}
'''
SUFFIX=r'''
int run(int mode){
 scenario=mode;uint64_t r=(uintptr_t)root;root[0]=(uintptr_t)vt;vt[5]=123;memcpy(memory+0x2f05458,&r,8);
 if(mode==7){
  static uint8_t panel[0x580],input[0x580];static uint64_t child[1];
  uint64_t pn=(uintptr_t)panel,it=(uintptr_t)input,nm=(uintptr_t)"CharacterCreateNamePanel",label=(uintptr_t)"InputText",one=1;
  memcpy(panel+8,&nm,8);child[0]=it;uint64_t cp=(uintptr_t)child;memcpy(panel+0x58,&cp,8);memcpy(panel+0x60,&one,8);memcpy(input+8,&label,8);
  uint64_t v=(uintptr_t)memory+0x20b8718,p=(uintptr_t)"Hero",n=4;memcpy(input,&v,8);memcpy(input+0x558,&p,8);memcpy(input+0x560,&n,8);
  xblr_ui_text_entered("Hero");if(fe_name_poll((uintptr_t)memory,pn,0))return 20;
  fe_name_panel(1);xblr_ui_text_entered("Hero");if(fe_name_poll((uintptr_t)memory,pn,0)!=1)return 21;
  if(fe_name_poll((uintptr_t)memory,pn,0))return 22;
  xblr_ui_text_entered("Other");if(fe_name_poll((uintptr_t)memory,pn,10)!=-1)return 23;
  if(fe_name_poll((uintptr_t)memory,pn,2011)!=0)return 24;
  xblr_ui_text_entered("Hero");fe_name_panel(0);if(fe_name_poll((uintptr_t)memory,pn,3000))return 25;
  fe_name_panel(1);xblr_ui_text_entered("Hero");v++;memcpy(input,&v,8);if(fe_name_poll((uintptr_t)memory,pn,3000)!=-1)return 26;
  return violations?27:0;
 }
 const char*name=mode==2?"OnlinePlayModeModal":mode==3?"TutorialTooltip":"MainMenuButtonRibbon";
 uint64_t msg[8]={ui_hash("PanelManager"),ui_hash("OpenPanel"),(uintptr_t)name};
 for(int i=0;i<3;i++)xblr_ui_observe((uintptr_t)msg);frames=1;uint64_t buttons=xblr_pump_ui_frontend();
 if(mode==4||mode==5)return buttons||logs?1:0;
 if(mode==1){if(buttons!=4||logs)return 2;frames=3;if(xblr_pump_ui_frontend()!=4)return 3;frames=4;if(xblr_pump_ui_frontend())return 4;frames=9;xblr_pump_ui_frontend();frames=28;if(xblr_pump_ui_frontend())return 11;frames=29;if(xblr_pump_ui_frontend()!=4)return 12;return violations?5:0;}
 if(mode==3){if(buttons!=2||logs)return 6;frames=4;return xblr_pump_ui_frontend()?7:0;}
 if(buttons||logs!=1||actions[0]!=ui_hash("ToCharacterCreate"))return 8;
 if(xblr_pump_ui_frontend()||logs!=1)return 9;
 xblr_ui_observe((uintptr_t)msg);frames=2;if(xblr_pump_ui_frontend()||logs!=1)return 13;
 return violations?10:0;
}
'''
def main(sdk,out,originals=None):
    root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
    prefix=PREFIX.replace('root[8]','root[16]')
    prefix=prefix.replace('static uint64_t xblr_invoke_guest(', 'void xblr_ui_observe(uint64_t);uint64_t xblr_pump_ui_frontend(void);\nstatic uint64_t xblr_invoke_guest(')
    prefix=prefix.replace('logs++;}return 0;', 'logs++;}xblr_ui_observe(a[1]);if(xblr_pump_ui_frontend())violations++;return 0;')
    code=prefix+STUBS+(root/'src/ui_touch_ohos.inc').read_text()+(root/'src/ui_frontend_ohos.inc').read_text()+SUFFIX
    c=out/'test.c';c.write_text(code);dll=out/'test.dll'
    argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:run','-I'+str(root/'src'),str(c),str(root/'tests/windows_gate_support.c'),'-o',str(dll)]
    p=subprocess.run(argv,capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr);assert not p.returncode,p.stderr
    names=['no save to offline create; duplicate messages coalesce; nested message pump does not recurse','save to select X pulse and reopen hidden list after 20 frames','online modal offline fallback','tutorial B pulse','script suppresses automation','explicit off suppresses automation','explicit on overrides script']
    names.append('name acceptance: panel required, exact widget/vtable acknowledgement, once only, timeout never accepts, close cancels')
    for i,name in enumerate(names):
        instance=out/f'case{i}.dll';shutil.copy2(dll,instance);rc=C.CDLL(str(instance)).run(i);assert rc==0,(name,rc)
    if originals:
        sys.path.insert(0,str(root/'tools'))
        from rewrite_harmony import ui_observation_site,op_body,dis
        raw=bytearray((originals/'main.original.image.bin').read_bytes());site=ui_observation_site('main',raw)
        assert site[0]==0xb1dc64 and ui_observation_site('sdk',raw) is None
        ins=dis(site[1],site[0]);assert ins.mnemonic=='stp' and ins.op_str=='x28, x27, [sp, #0x120]'
        (out/'observation-leaf.S').write_text('\n'.join(op_body(ins,'test_ui_observation')))
        raw[site[0]]^=1
        try:ui_observation_site('main',raw)
        except AssertionError:pass
        else:raise AssertionError('changed r32 UI prologue accepted')
        names.append('r32 observation instruction/version guard; wrong opcode rejected')
    r={'result':'PASS','scope':__doc__,'cases':names};(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);p.add_argument('--originals',type=Path);a=p.parse_args();main(a.sdk,a.output,a.originals)
