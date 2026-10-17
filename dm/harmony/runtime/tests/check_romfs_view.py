"""Compile and exercise the exact path-view C functions; no resource IO or writes."""
import argparse, ctypes as C, hashlib, json, subprocess
from pathlib import Path

def main(sdk, out):
    root=Path(__file__).resolve().parents[1]
    source=root/'src/services_ohos.c'
    text=source.read_text(encoding='utf-8')
    # Compile the production helper bodies verbatim, isolated from native-only
    # services. The integration host_path_ex still validates every path segment.
    body=text[text.index('static char configured_romfs['):text.index('void sw_prof_add(')]
    prefix='''#include <stddef.h>
#define PATH_MAX 4096
int strcmp(const char *,const char *);
int strncmp(const char *a,const char *b,size_t n) {
 while(n--){unsigned char x=*a++,y=*b++;if(x!=y)return x-y;if(!x)return 0;}return 0;
}
char *strchr(const char *p,int c) {do{if(*p==(char)c)return (char*)p;}while(*p++);return 0;}
'''
    suffix='''
void configure(int flat,const char *root) {
 unsigned i=0;while(root[i]&&i<4095){configured_romfs[i]=root[i];++i;}
 configured_romfs[i]=0;flat_romfs=flat;
}
'''
    out.mkdir(parents=True,exist_ok=False)
    (out/'path_view.c').write_text(prefix+body+suffix,encoding='utf-8')
    argv=[str(sdk/'llvm/bin/clang.exe'),'--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-O2','-shared','-nostdlib','-fuse-ld=lld','-Wl,/noentry,/nodefaultlib',
          '-Wl,/export:configure,/export:xblr_romfs_rest,/export:xblr_romfs_name,/export:xblr_romfs_is_root,/export:xblr_fs_guest_name_matches,/export:xblr_fs_roots_disjoint',str(out/'path_view.c'),str(root/'tests/windows_gate_support.c'),str(root/'tests/windows_loader_support.c'),'-o',str(out/'path_view.dll')]
    p=subprocess.run(argv,capture_output=True,text=True);(out/'build.log').write_text(p.stdout+p.stderr)
    (out/'command.json').write_text(json.dumps(argv,indent=2));assert p.returncode==0,p.stderr
    dll=C.CDLL(str(out/'path_view.dll'));dll.configure.argtypes=[C.c_int,C.c_char_p]
    for name in ('xblr_romfs_rest','xblr_romfs_name','xblr_romfs_is_root','xblr_fs_guest_name_matches','xblr_fs_roots_disjoint'):
        getattr(dll,name).argtypes=[C.c_char_p,C.c_char_p]
    dll.xblr_romfs_rest.restype=C.c_char_p;dll.xblr_romfs_name.restype=C.c_char_p
    rows=[]
    def check(name,a,b,expected):
        got=getattr(dll,name)(a.encode(),b.encode());got=got.decode() if isinstance(got,bytes) else got
        assert got==expected,(name,a,b,got,expected);rows.append({'function':name,'root':a,'input':b,'result':got})
    dll.configure(1,b'/app/game-data')
    for guest,want in [('/data/build_key.txt','/build_key.txt'),('/data/.build.config','/.build.config'),('/data/00-file.data.holes','/00-file.data.holes'),('/data',''),('/data/','/'),('data/archive_manifest.psv','/archive_manifest.psv'),('///data/file','/file'),('/','/'),('',''),('/database/a',None),('/build.config',None),('/datax',None)]:
        check('xblr_romfs_rest','/app/game-data',guest,want)
    check('xblr_romfs_rest','/app/saves','/data/a','/data/a')
    for guest,want in [('rom:/',1),('rom:',1),('rom:////',1),('rom:/data',0),('rom:/data/',0),('rom:/data/x',0),('bad',0)]:
        check('xblr_romfs_is_root','/app/game-data',guest,want)
    check('xblr_romfs_is_root','/app/saves','save:/',0)
    check('xblr_romfs_name','/app/game-data','build.config','.build.config')
    check('xblr_romfs_name','/app/game-data','archive_manifest.psv','archive_manifest.psv')
    check('xblr_romfs_name','/app/saves','build.config','build.config')
    dll.configure(0,b'/native/romfs')
    check('xblr_romfs_rest','/native/romfs','/data/file','/data/file')
    check('xblr_romfs_is_root','/native/romfs','rom:/',0)
    check('xblr_romfs_name','/native/romfs','build.config','build.config')
    for physical,guest,want in [
        ('/app/saves/hero','save:/hero',1),('/app/saves/hero','save:hero',1),
        ('/app/saves/hero','save:/hero/',1),('/app/saves/hero','save:/hero////',1),
        ('/app/saves/sub/hero','save:/sub/hero/',1),('/app/saves/hero','save:////hero',1),
        ('/app/saves/英雄','save:/英雄/',1),('/app/saves/hero','save:/Hero/',0),
        ('/app/saves/hero','save:/heroes/',0),('/app/saves/heroes','save:/hero/',0),
        ('/app/saves/hero','save:/',0),('/app/saves/hero','',0),
        ('/app/saves/'+'a'*768,'save:/'+'a'*768+'/',1),
        ('/app/saves/'+'a'*769,'save:/'+'a'*769+'/',0),
    ]:check('xblr_fs_guest_name_matches',physical,guest,want)
    for a,b,want in [('/app/game-data','/app/saves',1),('/app/saves','/app/saves.work',1),
        ('/app/saves.work','/app/saves.work',0),('/app/saves.work','/app/saves.work/resources',0),
        ('/app/saves.new','/app/saves.new/temp',0),('/app','/app/saves',0),
        ('/app/saves','/app/saves2',1),('/app/saves.work','/app/saves.worker',1),
        ('/','/app/saves',0),('/','/',0)]:
        check('xblr_fs_roots_disjoint',a,b,want);check('xblr_fs_roots_disjoint',b,a,want)
    result={'result':'PASS','cases':len(rows),'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'helper_bodies_sha256':hashlib.sha256(body.encode()).hexdigest(),'dll_sha256':hashlib.sha256((out/'path_view.dll').read_bytes()).hexdigest(),'scope':'compiled exact C path-view helper bodies on Windows; no Harmony filesystem execution','rows':rows}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:result[k] for k in ('result','cases','scope')}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('native_sdk',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();main(a.native_sdk.resolve(),a.output.resolve())
