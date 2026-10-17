"""Fault-inject the production save copy C, without touching real save files.

Instrumented POSIX calls model directory EOF, IO failure and descriptor lifetime.
The old readdir loop is a negative control: it accepts a partial snapshot.
This is not a Harmony filesystem/power-loss durability test.
"""
import argparse,ctypes as C,hashlib,json,subprocess
from pathlib import Path

PREFIX=r'''
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#define PATH_MAX 4096
#define EIO 5
#define EINTR 4
#define ENOMEM 12
#define EINVAL 22
#define ELOOP 40
#define ENOTSUP 95
#define ENAMETOOLONG 36
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 64
#define O_EXCL 128
#define O_DIRECTORY 65536
#define O_NOFOLLOW 131072
#define S_ISDIR(x) ((x)==1)
#define S_ISREG(x) ((x)==2)
typedef intptr_t ssize_t;
typedef void DIR;
struct stat {int st_mode;};
struct dirent {char d_name[64];};
static int errno;
typedef intptr_t (*posix_api)(int,uintptr_t,uintptr_t,size_t,int *);
static posix_api api;
static intptr_t call(int op,uintptr_t a,uintptr_t b,size_t n){return api(op,a,b,n,&errno);}
static int lstat(const char *p,struct stat *s){return (int)call(1,(uintptr_t)p,(uintptr_t)s,0);}
static int mkdir(const char *p,int m){return (int)call(2,(uintptr_t)p,0,(size_t)m);}
static DIR *opendir(const char *p){return (DIR*)call(3,(uintptr_t)p,0,0);}
static struct dirent *readdir(DIR *d){return (struct dirent*)call(4,(uintptr_t)d,0,0);}
static int closedir(DIR *d){return (int)call(5,(uintptr_t)d,0,0);}
static void *malloc(size_t n){return (void*)call(6,0,0,n);}
static void free(void *p){(void)call(7,(uintptr_t)p,0,0);}
static int open(const char *p,int flags,...){return (int)call(8,(uintptr_t)p,0,(size_t)flags);}
static ssize_t read(int f,void *p,size_t n){return call(9,(uintptr_t)f,(uintptr_t)p,n);}
static ssize_t write(int f,const void *p,size_t n){return call(10,(uintptr_t)f,(uintptr_t)p,n);}
static int fsync(int f){return (int)call(11,(uintptr_t)f,0,0);}
static int close(int f){return (int)call(12,(uintptr_t)f,0,0);}
static int strcmp(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return (unsigned char)*a-(unsigned char)*b;}
static int snprintf(char *out,size_t n,const char *fmt,...) {
 va_list args;va_start(args,fmt);size_t used=0;
 while(*fmt){if(*fmt=='%'&&fmt[1]=='s'){const char *p=va_arg(args,const char*);fmt+=2;
   while(*p){if(used+1<n)out[used]=*p;++used;++p;}}
  else{if(used+1<n)out[used]=*fmt;++used;++fmt;}}
 if(n)out[used<n?used:n-1]=0;va_end(args);return (int)used;
}
'''
SUFFIX=r'''
int run_copy(posix_api dispatch,unsigned depth,int *report){
 api=dispatch;errno=123;int result=copy_tree("/from","/to",depth);
 report[0]=errno;report[1]=result==0;return result;
}
'''

API=C.CFUNCTYPE(C.c_ssize_t,C.c_int,C.c_size_t,C.c_size_t,C.c_size_t,C.POINTER(C.c_int))

class FakeFS:
    def __init__(self,mode):
        self.mode=mode;self.allocs={};self.dirs={};self.fds={};self.output={};self.events=[]
        self.entries=[C.create_string_buffer(x.encode(),64) for x in ('.','..','a','sub','b')]
        self.faulted=False;self.callback_error=None;self.violations=[];self.synced=[];self.serial=100
    def fault(self,err,code=-1):
        self.err[0]=err;self.faulted=True;return code
    def invoke(self,op,a,b,n,err):
        try:return self._invoke(op,a,b,n,err)
        except BaseException as exc:self.callback_error=repr(exc);err[0]=5;return -1
    def _invoke(self,op,a,b,n,err):
        self.err=err
        p=C.string_at(a).decode() if op in (1,2,3,8) else ''
        self.events.append((op,p if p else a))
        if op==1:
            if self.mode=='lstat failure' and p.endswith('/b'):return self.fault(13)
            C.cast(b,C.POINTER(C.c_int))[0]=1 if p in ('/from','/from/sub') else 2
            if self.mode=='unsupported symlink' and p.endswith('/a'):C.cast(b,C.POINTER(C.c_int))[0]=3
            return 0
        if op==2:
            if self.mode=='mkdir failure':return self.fault(28)
            return 0
        if op==3:
            if self.mode=='opendir failure':return self.fault(13,0)
            self.serial+=1;self.dirs[self.serial]=[p,0];return self.serial
        if op==4:
            p,i=self.dirs[a]
            if ((self.mode=='root readdir failure' and p=='/from') or
                (self.mode=='nested readdir failure' and p=='/from/sub')) and i==3:return self.fault(5,0)
            values=[0,1] if self.mode=='empty tree' else [0,1,2,3] if p=='/from' else [0,1,4]
            if i>=len(values):return 0
            self.dirs[a][1]+=1;return C.addressof(self.entries[values[i]])
        if op==5:
            del self.dirs[a]
            if self.mode=='closedir failure':return self.fault(5)
            return 0
        if op==6:
            if self.mode==('directory allocation failure' if n==8192 else 'file allocation failure'):return self.fault(12,0)
            buf=C.create_string_buffer(n);addr=C.addressof(buf);self.allocs[addr]=buf;return addr
        if op==7:
            if a:del self.allocs[a]
            return 0
        if op==8:
            if self.mode=='open directory sync failure' and n&65536:return self.fault(13)
            if self.mode=='open input failure' and p=='/from/a':return self.fault(13)
            if self.mode=='open output failure' and p=='/to/a':return self.fault(28)
            self.serial+=1;self.fds[self.serial]=[p,n,0]
            if n&1:self.output[p]=bytearray()
            return self.serial
        if op==9:
            p,flags,pos=self.fds[a]
            if self.mode=='read failure':return self.fault(5)
            if self.mode=='read interruption' and not self.faulted:return self.fault(4)
            data=(b'hero-data-12345' if p.endswith('/a') else b'nested-save-bytes')[pos:pos+n]
            C.memmove(b,data,len(data));self.fds[a][2]+=len(data);return len(data)
        if op==10:
            p,flags,pos=self.fds[a]
            if self.mode=='write failure':return self.fault(28)
            if self.mode=='zero write':self.faulted=True;return 0
            if self.mode=='write interruption' and not self.faulted:return self.fault(4)
            if self.mode=='partial writes':n=min(n,3)
            self.output[p]+=C.string_at(b,n);return n
        if op==11:
            p,flags,pos=self.fds[a]
            if self.mode==('directory fsync failure' if flags&65536 else 'file fsync failure'):return self.fault(5)
            self.synced.append(p);return 0
        if op==12:
            p,flags,pos=self.fds.pop(a)
            category='directory close failure' if flags&65536 else 'output close failure' if flags&1 else 'input close failure'
            if self.mode==category:return self.fault(5)
            return 0
        raise AssertionError(('unknown operation',op))

def main(sdk,out):
    root=Path(__file__).resolve().parents[1];source=root/'src/services_ohos.c'
    text=source.read_text(encoding='utf-8');body=text[text.index('static int copy_tree('):text.index('int xblr_clone_tree(')]
    marker='''for(;;) {
            /* readdir uses NULL for both EOF and errors. A truncated directory
             * must never be accepted as a complete save transaction snapshot. */
            errno=0;e=readdir(d);
            if(!e){if(errno)result=-1;break;}'''
    assert body.count(marker)==1
    old=body.replace(marker,'while((e=readdir(d))) {')
    out.mkdir(parents=True,exist_ok=False);commands=[];dlls=[]
    for name,code in [('save_copy',body),('save_old',old)]:
        c=out/(name+'.c');c.write_text(PREFIX+code+SUFFIX,encoding='utf-8')
        argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-O2','-shared','-nostdlib','-fuse-ld=lld',
            '-Wl,/noentry,/nodefaultlib,/export:run_copy',str(c),'-o',str(out/(name+'.dll'))]
        p=subprocess.run(argv,capture_output=True,text=True);commands.append(argv)
        (out/(name+'.log')).write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
        dll=C.CDLL(str(out/(name+'.dll')));dll.run_copy.argtypes=[API,C.c_uint,C.POINTER(C.c_int)];dlls.append(dll)
    (out/'commands.json').write_text(json.dumps(commands,indent=2))
    rows=[]
    def run(dll,mode,expected,depth=0):
        fs=FakeFS(mode);callback=API(fs.invoke);report=(C.c_int*2)();result=dll.run_copy(callback,depth,report)
        assert not fs.callback_error,(mode,fs.callback_error)
        assert result==expected,(mode,result,list(report))
        assert not fs.allocs and not fs.dirs and not fs.fds,(mode,'resource leak',fs.allocs,fs.dirs,fs.fds)
        if expected:assert report[1]==0 and report[0]>0,(mode,list(report))
        elif mode!='empty tree':
            assert fs.output=={'/to/a':b'hero-data-12345','/to/sub/b':b'nested-save-bytes'},(mode,fs.output)
            assert fs.synced==['/to/a','/to/sub/b','/to/sub','/to'],(mode,fs.synced)
        row={'name':mode,'result':result,'errno':report[0],'commit_allowed':bool(report[1]),'synced':fs.synced}
        return fs,row
    for name in ('normal nested tree','empty tree','read interruption','write interruption','partial writes'):
        rows.append(run(dlls[0],name,0)[1])
    for name in ('root readdir failure','nested readdir failure','closedir failure','directory allocation failure','file allocation failure',
        'mkdir failure','opendir failure','open directory sync failure','open input failure','open output failure',
        'read failure','write failure','zero write','file fsync failure','directory fsync failure',
        'directory close failure','input close failure','output close failure','lstat failure','unsupported symlink'):
        rows.append(run(dlls[0],name,-1)[1])
    rows.append(run(dlls[0],'recursion depth limit',-1,33)[1])
    oldfs=FakeFS('root readdir failure');oldcb=API(oldfs.invoke);report=(C.c_int*2)();result=dlls[1].run_copy(oldcb,0,report)
    assert not oldfs.callback_error and result==0 and report[1]==1 and '/to/sub/b' not in oldfs.output
    negative={'result':result,'errno':report[0],'commit_allowed':bool(report[1]),'copied_files':list(oldfs.output),
        'failure':'old loop accepts incomplete snapshot after injected readdir EIO'}
    result={'result':'PASS','cases':len(rows),'scope':'exact production C copy helper and scripted POSIX failure paths; no real save files/device IO',
        'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'rows':rows,'negative_control':negative}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:result[k] for k in ('result','cases','scope','negative_control')}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('native_sdk',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();main(a.native_sdk.resolve(),a.output.resolve())
