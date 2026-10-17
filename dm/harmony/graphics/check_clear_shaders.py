"""Read generated SPIR-V and interpret its small straight-line clear programs.
This checks values/locations independently of the generator, not GPU execution.
"""
import argparse,struct,json,re
from pathlib import Path

def run(words,vertex=0,push=()):
    values={};memory={};types={};decorations={};output=[]
    p=5
    while p<len(words):
        size=words[p]>>16;op=words[p]&65535;a=words[p+1:p+size];p+=size
        if op==71:decorations.setdefault(a[0],{})[a[1]]=a[2:]
        elif op==22:types[a[0]]="float"
        elif op==21:types[a[0]]="int" if a[2] else "uint"
        elif op==43:
            v=a[2];t=types[a[0]]
            if t=="float":v=struct.unpack("<f",struct.pack("<I",v))[0]
            elif t=="int" and v&0x80000000:v-=1<<32
            values[a[1]]=v
        elif op==59:
            ident,storage=a[1],a[2];values[ident]=(ident,)
            if storage==1:memory[ident]=vertex
            elif storage==9:memory[ident]=[tuple(push)]
            elif storage==3:memory[ident]=None;output.append(ident)
        elif op==65:values[a[1]]=values[a[2]]+tuple(values[i] for i in a[3:])
        elif op==61:
            pointer=values[a[2]];v=memory[pointer[0]]
            for i in pointer[1:]:v=v[i]
            values[a[1]]=v
        elif op==62:memory[values[a[0]][0]]=values[a[1]]
        elif op==170:values[a[1]]=values[a[2]]==values[a[3]]
        elif op==169:values[a[1]]=values[a[3] if values[a[2]] else a[4]]
        elif op==80:values[a[1]]=tuple(values[i] for i in a[2:])
    return [(decorations[i],memory[i]) for i in output]
if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("--output",type=Path,required=True);a=p.parse_args()
    text=Path(__file__).with_name("xbg_clear_spv.h").read_text();arrays={name:[int(x.strip(),0) for x in body.split(",") if x.strip()] for name,body in re.findall(r"static const uint32_t (\w+)\[\]=\{(.*?)\};",text,re.S)}
    vs=arrays["xbg_clear_vs"];expected=[(-1.,-1.,0.,1.),(3.,-1.,0.,1.),(-1.,3.,0.,1.)]
    actual=[run(vs,i)[0][1] for i in range(3)];assert actual==expected
    # All viewport pixel centers in NDC lie inside the emitted triangle.
    for x in [-.75,-.25,.25,.75]:
        for y in [-.75,-.25,.25,.75]:
            for i in range(3):
                x0,y0,*_=actual[i];x1,y1,*_=actual[(i+1)%3];assert (x1-x0)*(y-y0)-(y1-y0)*(x-x0)>0
    offset=int(re.search(r"xbg_clear_fs_location_word=(\d+)",text).group(1));checks=0
    for kind,data in [("float",[-.125,.5,2.,-3.]),("sint",[-1,-2147483648,2147483647,123]),("uint",[4294967295,2147483648,77,0])]:
        for location in range(8):
            words=arrays["xbg_clear_fs_"+kind].copy();words[offset]=location;result=run(words,push=data)
            assert result[0][0][30]==[location] and result[0][1]==tuple(data);checks+=1
    # Negative control changes the actual SPIR-V triangle, not the oracle.
    bad=vs.copy();negative=False
    for i in range(5,len(bad)-3):
        if bad[i]==(4<<16)|43 and bad[i+2]==12:bad[i+3]=0;break
    try:assert [run(bad,i)[0][1] for i in range(3)]==expected
    except AssertionError:negative=True
    assert negative
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps({"vertex_positions":actual,"viewport_centers":16,"typed_location_cases":checks,"negative_vertex_constant_detected":negative},indent=2));print("PASS: triangle coverage, 24 typed/location cases, SPIR-V negative control")
