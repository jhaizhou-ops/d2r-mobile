"""Bounded host tests for the production masked-clear planner; no GPU/device."""
import argparse,ctypes,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("--output",type=Path,required=True);p.add_argument("--shim",type=Path,required=True);p.add_argument("--llvm",type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);src=Path(__file__).parent

def compile(source,obj):
    subprocess.run([str(a.llvm/"clang.exe"),"-target","x86_64-pc-windows-msvc","-ffreestanding","-fno-builtin","-O2","-I"+str(a.shim),"-I"+str(src),"-c",str(source),"-o",str(obj)],check=True)
compile(src/"check_clear.cpp",a.output/"test.obj");results=[]
for name,old,new,expected in [
    ("production","","",0),
    ("color","c.colorWriteMask=cm","c.colorWriteMask=15",1),
    ("stencil","d.front.writeMask=sm","d.front.writeMask=255",4),
    ("bounds","right=int64_t(x)+w","right=int32_t(uint32_t(x)+w)",8),
    ("type","if(t==3 || t==8 || t==11)return 2","if(t==3 || t==8 || t==11)return 1",12),
    ("layer","*layer=base+uint32_t(z)+slice","*layer=uint32_t(z)+slice",16),
    ("preserve","!r || partial || levels>1","!r || false || levels>1",20)]:
    text=(src/"xbg_clear.cpp").read_text()
    if old:
        assert old in text;text=text.replace(old,new)
    source=a.output/(name+".cpp");source.write_text(text);obj=a.output/(name+".obj");compile(source,obj)
    dll=a.output/(name+".dll")
    subprocess.run([str(a.llvm/"lld-link.exe"),"/dll","/noentry","/nodefaultlib","/machine:x64","/out:"+str(dll),"/export:xbg_clear_selftest",str(obj),str(a.output/"test.obj"),str(a.shim/"unit5.obj")],check=True)
    actual=ctypes.CDLL(str(dll)).xbg_clear_selftest();assert actual==expected,(name,actual,expected);results.append({"name":name,"actual":actual,"expected":expected})
(a.output/"cpu-check.json").write_text(json.dumps({"groups":21,"color_masks":16,"stencil_masks":256,"faces":2,"results":results},indent=2));print("PASS: 21 groups, 6 negative controls")
