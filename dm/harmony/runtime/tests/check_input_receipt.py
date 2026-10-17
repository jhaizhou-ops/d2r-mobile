"""Compile production generated HID getter/fill and bounded receipt on Windows.

The real GetNpadState wrappers run with an instrumented single-thread mutex,
clock and log sink. This tests observation and limits, not device concurrency,
frontend gestures, or the game's reaction to a returned button.
"""
import argparse,ctypes as C,hashlib,json,re,subprocess,sys
from pathlib import Path

PREFIX=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdatomic.h>
#define PTHREAD_MUTEX_INITIALIZER 0
#define XBLR_EXEC_ARGUMENT 40
#define stderr ((void*)1)
typedef int pthread_mutex_t;
void *memset(void*,int,size_t);
void *memcpy(void*,const void*,size_t);
static int memcmp(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]-y[i];return 0;}
static int violations,mode=1,clock_calls,log_count;
static uint64_t time_ns=100,logs[512][13];
static void pthread_mutex_lock(int *m){if(*m)++violations;*m=1;}
static void pthread_mutex_unlock(int *m){if(!*m)++violations;*m=0;}
static uint64_t now_ns(void){++clock_calls;return time_ns;}
static int sw_opmode(void){return mode;}
static void xblr_pump_ui_touch(void){}
static void xblr_world_note_stick(int32_t lx,int32_t ly){(void)lx;(void)ly;}
static uint64_t xblr_pump_ui_frontend(void){return 0;}
static int receipt_printf(void *file,const char *format,...){
 (void)file;(void)format;if(log_count>=512){++violations;return 0;}
 uint64_t *r=logs[log_count++];va_list a;va_start(a,format);
 r[0]=va_arg(a,unsigned long long);r[1]=va_arg(a,unsigned int);r[2]=va_arg(a,unsigned int);
 r[3]=va_arg(a,unsigned long long);r[4]=(uint64_t)va_arg(a,long long);
 r[5]=va_arg(a,unsigned int);r[6]=va_arg(a,unsigned int);r[7]=va_arg(a,unsigned long long);
 for(int i=8;i<12;i++)r[i]=(uint64_t)(int64_t)va_arg(a,int);
 r[12]=va_arg(a,unsigned int);va_end(a);return 0;
}
#define fprintf receipt_printf
#define HLE_FORCE(symbol,description,ret,name,...) ret name(__VA_ARGS__)
'''
SUFFIX=r'''
void test_reset(void){
 input=(NpadState){0};input_lock=0;supported_styles=3;supported_ids=1ull|(1ull<<NPAD_HANDHELD);
 memset(input_receipt_slots,0,sizeof input_receipt_slots);input_receipt_generation=0;
 input_receipt_remaining=input_receipt_records=0;violations=clock_calls=log_count=0;time_ns=100;mode=1;
}
void test_clock(uint64_t ns){time_ns=ns;}
void test_mode(int m){mode=m;}
void test_supported(uint64_t ids,uint32_t styles){supported_ids=ids;supported_styles=styles;}
void test_read(uint32_t id,uint32_t style,NpadState *s){
 if(style==STYLE_FULLKEY)h_hid_GetFullKey(s,&id);else h_hid_GetHandheld(s,&id);
}
uint64_t test_meta(int which){switch(which){case 0:return log_count;case 1:return clock_calls;case 2:return violations;case 3:return input_receipt_remaining;case 4:return input_receipt_generation;}return 0;}
void test_record(int index,uint64_t *out){memcpy(out,logs[index],sizeof logs[index]);}
'''

class State(C.Structure):
    _fields_=[('sampling',C.c_int64),('buttons',C.c_uint64),('lx',C.c_int32),('ly',C.c_int32),('rx',C.c_int32),('ry',C.c_int32),('attributes',C.c_uint32),('pad',C.c_uint32)]

def main(sdk,out):
    root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'tools'))
    from prepare_services import prepare
    out.mkdir(parents=True,exist_ok=False);gen=out/'generated';gen.mkdir();prepare(root,gen)
    source=gen/'hle_hid_ohos.c';generated=source.read_text(encoding='utf-8')
    pre=generated[:generated.index('HLE_FORCE(')].replace('#include "hle_ohos_compat.h"','')
    helper=root/'src/input_receipt_ohos.inc'
    pre=pre.replace('#include "input_receipt_ohos.inc"',helper.read_text(encoding='utf-8'))
    wrappers='\n'.join(line for line in generated.splitlines() if line.startswith('HLE_FORCE(') and any(n in line for n in ('h_hid_GetFullKey,','h_hid_GetHandheld,')))
    assert wrappers.count('fill(s, *id,')==2
    body=PREFIX+pre+wrappers+SUFFIX;commands=[];dlls=[]
    for name,code in [('actual',body),('host_logging_negative',body.replace('input.ry=axis(ry);pthread_mutex_unlock','input.ry=axis(ry);input_receipt_return(&input,0,STYLE_FULLKEY);pthread_mutex_unlock'))]:
        c=out/(name+'.c');c.write_text(code,encoding='utf-8')
        exports=['test_reset','test_clock','test_mode','test_supported','test_read','test_meta','test_record','xblr_set_input','xblr_set_input_receipt_budget','xblr_get_input_samples']
        argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-O2','-shared','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib',*[f'-Wl,/export:{n}' for n in exports],str(c),str(root/'tests/windows_gate_support.c'),'-o',str(out/(name+'.dll'))]
        p=subprocess.run(argv,capture_output=True,text=True);commands.append(argv);(out/(name+'.log')).write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
        d=C.CDLL(str(out/(name+'.dll')));d.test_meta.argtypes=[C.c_int];d.test_meta.restype=C.c_uint64
        d.test_clock.argtypes=[C.c_uint64];d.test_supported.argtypes=[C.c_uint64,C.c_uint32]
        d.test_read.argtypes=[C.c_uint32,C.c_uint32,C.POINTER(State)]
        d.xblr_set_input.argtypes=[C.c_uint64,C.c_int32,C.c_int32,C.c_int32,C.c_int32]
        d.xblr_get_input_samples.restype=C.c_uint64
        d.xblr_set_input_receipt_budget.argtypes=[C.c_uint32];d.test_record.argtypes=[C.c_int,C.POINTER(C.c_uint64)]
        dlls.append(d)
    (out/'commands.json').write_text(json.dumps(commands,indent=2))
    d,negative=dlls;rows=[]
    baseline=d.xblr_get_input_samples();assert baseline==0
    for i in range(8):d.xblr_set_input(i,0,0,0,0)
    d.xblr_set_input_receipt_budget(4);d.xblr_set_input_receipt_budget(0)
    assert d.xblr_get_input_samples()==baseline
    sample=State()
    for id,style in [(0,1),(32,2),(0xffffffff,1)]:d.test_read(id,style,C.byref(sample))
    assert d.xblr_get_input_samples()==baseline+3
    d.test_reset();assert d.xblr_get_input_samples()==baseline+3
    rows.append({'name':'samples count actual getters including disconnected; publishing budget and reset do not advance or rewind'})
    def read(id=0,style=1):
        s=State();d.test_read(id,style,C.byref(s));return s
    def record(index=-1):
        r=(C.c_uint64*13)();d.test_record(d.test_meta(0)-1 if index<0 else index,r);return list(r)
    def case(name):
        assert d.test_meta(2)==0,name;rows.append({'name':name,'records':d.test_meta(0),'remaining':d.test_meta(3)});d.test_reset()
    d.test_reset();d.xblr_set_input(1,0,0,0,0);s=read();assert s.buttons==1 and d.test_meta(0)==d.test_meta(1)==0
    case('default off has no records or clock calls')
    assert d.xblr_set_input_receipt_budget(8)==0;d.xblr_set_input(1,0,0,0,0);assert d.test_meta(0)==0
    s=read();assert record()[7:]==[1,0,0,0,0,1] and record()[4]==s.sampling
    d.xblr_set_input(0,0,0,0,0);s=read();assert record()[7]==0 and record()[4]==s.sampling
    case('only actual getter records down and up')
    d.xblr_set_input_receipt_budget(8);read();d.xblr_set_input(1,0,0,0,0);d.xblr_set_input(0,0,0,0,0);read();assert d.test_meta(0)==1
    case('unobserved short tap is not fabricated')
    d.xblr_set_input_receipt_budget(8);read();d.test_clock(999999999);read();assert d.test_meta(0)==1
    d.test_clock(1000000100);read();assert d.test_meta(0)==2;read();assert d.test_meta(0)==2
    case('sampling-only polls suppressed and one-second witness bounded')
    d.xblr_set_input_receipt_budget(8);d.xblr_set_input(0x1003,40000,-40000,123,-456);s=read()
    assert s.buttons==0x1003 and (s.lx,s.ly,s.rx,s.ry)==(32767,-32767,123,-456)
    assert record()[8:12]==[32767,(1<<64)-32767,123,(1<<64)-456]
    d.xblr_set_input(0,0,0,0,0);s=read();assert record()[7:12]==[0]*5
    case('merged buttons axes clamping and full release preserved')
    d.xblr_set_input_receipt_budget(12);d.xblr_set_input(3,10,20,30,40);s=read(32,2)
    assert s.buttons==s.attributes==0 and record()[7:]==[0]*6
    s=read();assert s.buttons==3 and s.attributes==1
    for _ in range(10):read(32,2);read()
    assert d.test_meta(0)==2
    d.test_clock(2000000000);read(32,2);assert d.test_meta(0)==2
    case('disconnected pair reports returned zeros and does not flood alternating queries')
    d.xblr_set_input_receipt_budget(8);d.test_mode(0);d.xblr_set_input(2,0,0,0,0);s=read(32,2);assert s.buttons==2 and s.attributes==1
    d.test_supported(1<<32,1);s=read(32,2);assert s.buttons==0 and s.attributes==0
    case('handheld mode and supported-style filtering unchanged')
    d.xblr_set_input_receipt_budget(4);d.xblr_set_input(3,0,0,0,0);s=read(0xffffffff,1);assert s.buttons==s.attributes==0 and record()[5]==0xffffffff
    case('invalid id has bounded safe observation')
    assert d.xblr_set_input_receipt_budget(257)==40 and d.test_meta(4)==0
    d.xblr_set_input_receipt_budget(3);read();assert d.xblr_set_input_receipt_budget(257)==40 and d.test_meta(3)==2 and d.test_meta(4)==1
    case('invalid budget leaves active generation unchanged')
    d.xblr_set_input_receipt_budget(3)
    for i in range(10):d.xblr_set_input(i&1,0,0,0,0);read()
    assert d.test_meta(0)==3 and d.test_meta(3)==0 and record()[1:3]==[3,0]
    c=d.test_meta(1);read();assert d.test_meta(1)==c
    case('budget exhausts exactly and disables clock work')
    d.xblr_set_input_receipt_budget(256)
    for i in range(300):d.xblr_set_input(i&1,0,0,0,0);read()
    assert d.test_meta(0)==256 and record()[1:3]==[256,0]
    case('maximum budget bounded to 256 actual records')
    d.xblr_set_input_receipt_budget(4);read();d.xblr_set_input_receipt_budget(0);d.xblr_set_input(2,0,0,0,0);s=read();assert s.buttons==2 and d.test_meta(0)==1
    d.xblr_set_input_receipt_budget(2);read();assert record()[0:3]==[3,1,1]
    case('manual disable and rearm preserve input and distinguish generations')
    negative.test_reset();negative.xblr_set_input_receipt_budget(4);negative.xblr_set_input(1,0,0,0,0)
    assert negative.test_meta(0)==1
    result={'result':'PASS','cases':len(rows),'scope':__doc__.strip(),'rows':rows,
            'negative_control':'A receipt incorrectly placed in host set_input emits a record without any guest getter; observed 1 and rejected.',
            'generated_hid_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest()}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('sdk',type=Path);p.add_argument('output',type=Path);a=p.parse_args();main(a.sdk,a.output)
