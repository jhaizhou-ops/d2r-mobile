"""Production calendar conversion, deterministic host success/failure injection.
Checks error propagation and untouched output, not Harmony timezone behavior.
"""
import argparse,ctypes as C,json,re,subprocess,sys
from pathlib import Path

PREFIX=r'''
#include <stdint.h>
#include <stddef.h>
typedef int Result;
typedef int64_t time_t;
#define RES_OK 0
#define RES_FAIL 1
#define HLE_FORCE(symbol,description,ret,name,...) ret name(__VA_ARGS__)
void *memset(void*,int,size_t);
struct tm {int tm_year,tm_mon,tm_mday,tm_hour,tm_min,tm_sec,tm_wday,tm_yday,tm_isdst;const char *tm_zone;long tm_gmtoff;};
static int fail;
static struct tm *localtime_r(const time_t *t,struct tm *v){
 (void)t;*v=(struct tm){126,9,10,8,47,1,6,282,0,"CST",28800};return fail?0:v;
}
static size_t strlcpy(char *d,const char *s,size_t n){size_t i=0;while(s[i])i++;for(size_t k=0;k<n;k++){d[k]=k<i?s[k]:0;}if(n)d[n-1]=0;return i;}
'''
SUFFIX=r'''
int test_run(int which){
 CalendarTime ct;CalendarAdditionalInfo ai;int64_t t=1791593221;
 memset(&ct,0x5a,sizeof ct);memset(&ai,0x5a,sizeof ai);fail=which==1;
 Result r=h_time_ToCalendarTime(which==2?0:&ct,which==3?0:&ai,&t);
 if(which==1){
  unsigned char *p=(void*)&ct,*q=(void*)&ai;
  if(r==RES_OK)return 0;
  for(size_t i=0;i<sizeof ct;i++)if(p[i]!=0x5a)return 0;
  for(size_t i=0;i<sizeof ai;i++)if(q[i]!=0x5a)return 0;return 1;
 }
 if(r)return 0;
 if(which!=2&&(ct.year!=2026||ct.month!=10||ct.day!=10||ct.hour!=8||ct.minute!=47||ct.second!=1||ct.pad))return 0;
 if(which!=3&&(ai.wday!=6||ai.yday!=282||ai.offset!=28800||ai.dst||ai.tzname[0]!='C'))return 0;
 return 1;
}
'''

def main(sdk,out):
    root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'tools'))
    from prepare_services import prepare
    out.mkdir(parents=True,exist_ok=False);gen=out/'generated';gen.mkdir();prepare(root,gen)
    text=(gen/'hle_time_ohos.c').read_text(encoding='utf-8')
    types='\n'.join(re.findall(r'^typedef struct.*;',text,re.M))
    function=text[text.index('HLE_FORCE("_ZN2nn4time14ToCalendarTime'):]
    body=PREFIX+types+'\n'+function+SUFFIX;results=[]
    for name,code in [('actual',body),('unchecked_negative',body.replace('if(!localtime_r(&t, &tm))return RES_FAIL;','localtime_r(&t, &tm);'))]:
        source=out/(name+'.c');source.write_text(code,encoding='utf-8');dll=out/(name+'.dll')
        argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-O2','-shared','-ffreestanding','-fno-builtin','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib,/export:test_run',str(source),str(root/'tests/windows_gate_support.c'),'-o',str(dll)]
        p=subprocess.run(argv,capture_output=True,text=True);(out/(name+'.log')).write_text(p.stdout+p.stderr);assert not p.returncode,p.stderr
        lib=C.CDLL(str(dll));lib.test_run.argtypes=[C.c_int];lib.test_run.restype=C.c_int
        got=[lib.test_run(i) for i in range(4)];assert got==([1]*4 if name=='actual' else [1,0,1,1]),got
        results.append({'variant':name,'cases':got,'command':argv})
    result={'result':'PASS','scope':__doc__,'cases':['success fields','host failure leaves both outputs','null calendar output','null additional output'],'negative_control':'old unchecked failure incorrectly succeeds; rejected','variants':results}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({'result':'PASS','cases':4,'negative_control':'rejected'}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
