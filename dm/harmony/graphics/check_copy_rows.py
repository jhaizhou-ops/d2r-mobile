"""Bounded host tests for the production 2D/array blit planner; no GPU/device."""
import argparse,ctypes,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("--output",type=Path,required=True);p.add_argument("--shim",type=Path,required=True);p.add_argument("--llvm",type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);src=Path(__file__).parent

def compile(source,obj):
    subprocess.run([str(a.llvm/"clang.exe"),"-target","x86_64-pc-windows-msvc","-ffreestanding","-fno-builtin","-O2","-I"+str(a.shim),"-I"+str(src),"-c",str(source),"-o",str(obj)],check=True)
compile(src/"check_copy_rows.cpp",a.output/"test.obj");results=[]
for name,old,new,expected in [
    ("production","","",0),
    ("tail","(rows-1)*row+tight","rows*row",1),
    ("padding","size_t(p.tight)","size_t(p.row)",3),
    ("destination_bound","dst_bytes<p.bytes","false",4),
    ("source_bound","src_bytes<p.bytes","false",6),
    ("preserve_mips","r.levels==1 && ","",14)]:
    text=(src/"xbg_blit.cpp").read_text()
    if old:
        assert old in text;text=text.replace(old,new)
    source=a.output/(name+".cpp");source.write_text(text);obj=a.output/(name+".obj");compile(source,obj)
    dll=a.output/(name+".dll")
    subprocess.run([str(a.llvm/"lld-link.exe"),"/dll","/noentry","/nodefaultlib","/machine:x64","/out:"+str(dll),"/export:xbg_copy_rows_selftest",str(obj),str(a.output/"test.obj"),str(a.shim/"unit5.obj")],check=True)
    actual=ctypes.CDLL(str(dll)).xbg_copy_rows_selftest();assert actual==expected,(name,actual,expected);results.append({"name":name,"actual":actual,"expected":expected})
(a.output/"cpu-check.json").write_text(json.dumps({"groups":18,"results":results},indent=2));print("PASS: 18 groups, 5 negative controls")
