"""Bounded host tests for the production masked-clear planner; no GPU/device."""
import argparse,ctypes,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("--output",type=Path,required=True);p.add_argument("--shim",type=Path,required=True);p.add_argument("--llvm",type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);src=Path(__file__).parent

def compile(source,obj):
    subprocess.run([str(a.llvm/"clang.exe"),"-target","x86_64-pc-windows-msvc","-ffreestanding","-fno-builtin","-O2","-I"+str(a.shim),"-I"+str(src),"-c",str(source),"-o",str(obj)],check=True)
compile(src/"check_clear_value.cpp",a.output/"test.obj");results=[]
for name,old,new,expected in [
    ("production","","",0),
    ("raw_union","value.float32[i]=float(x)","value.uint32[i]=source.uint32[i]",2),
    ("signedness","case VK_FORMAT_R32G32B32A32_UINT:case", "case",4),
    ("wrong_view","int target=xbg_clear_vk_class(format)","int target=0",3)]:
    text=(src/"xbg_clear.cpp").read_text()
    if old:
        assert old in text;text=text.replace(old,new)
    source=a.output/(name+".cpp");source.write_text(text);obj=a.output/(name+".obj");compile(source,obj)
    dll=a.output/(name+".dll")
    subprocess.run([str(a.llvm/"lld-link.exe"),"/dll","/noentry","/nodefaultlib","/machine:x64","/out:"+str(dll),"/export:xbg_clear_value_selftest",str(obj),str(a.output/"test.obj"),str(a.shim/"unit5.obj")],check=True)
    actual=ctypes.CDLL(str(dll)).xbg_clear_value_selftest();assert actual==expected,(name,actual,expected);results.append({"name":name,"actual":actual,"expected":expected})
(a.output/"cpu-check.json").write_text(json.dumps({"groups":15,"results":results},indent=2));print("PASS: 15 groups, 3 negative controls")
