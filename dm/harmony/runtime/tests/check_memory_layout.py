"""Exercise compiled OHOS AArch64 free-range code under Unicorn, not an OS mock."""
import argparse,json,struct,subprocess
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X30,UC_ARM64_REG_SP

def main(out,sdk):
    root=Path(__file__).resolve().parents[1];out.mkdir(parents=True,exist_ok=False)
    obj=out/'memory_layout.o';code=out/'memory_layout.text'
    commands=[
      [sdk/'llvm/bin/clang.exe','--target=aarch64-linux-ohos','-O2','-fno-stack-protector','-ffixed-x18','-c',root/'src/memory_layout.c','-o',obj],
      [sdk/'llvm/bin/llvm-objcopy.exe','-O','binary','--only-section=.text',obj,code]]
    for c in commands:subprocess.run(list(map(str,c)),check=True,capture_output=True)
    limit=1<<39;regions=[(0x5000,0x3000),(0x1000,0x1000),(0x4000,0x1000)]
    cases=[(0,regions,(1,0,0x1000,0)),(0xfff,regions,(1,0,0x1000,0)),
      (0x1000,regions,(0,)),(0x1fff,regions,(0,)),(0x2000,regions,(1,0x2000,0x2000,0)),
      (0x3fff,regions,(1,0x2000,0x2000,0)),(0x4000,regions,(0,)),(0x4fff,regions,(0,)),
      (0x5000,regions,(0,)),(0x7fff,regions,(0,)),(0x8000,regions,(1,0x8000,limit-0x8000,0)),
      (limit-1,regions,(1,0x8000,limit-0x8000,0)),(limit,regions,(1,limit,(1<<64)-limit,0x10)),
      ((1<<64)-1,regions,(1,limit,(1<<64)-limit,0x10)),(0,[],(1,0,limit,0)),
      (0,[(0x1234,0)],(1,0,limit,0)),(0,[(limit,1)],(-1,)),(0,[(limit-1,2)],(-1,)),
      (0,[(limit-0x1000,0x1000)],(1,0,limit-0x1000,0))]
    receipts=[]
    for a,rs,expected in cases:
        u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x100000,0x10000);u.mem_map(0x200000,0x10000)
        u.mem_write(0x100000,code.read_bytes())
        if rs:u.mem_write(0x200000,b''.join(struct.pack('<QQ',*r) for r in rs))
        for i,v in enumerate([a,0x200000,len(rs),0x201000,0x201008,0x201010]):u.reg_write(UC_ARM64_REG_X0+i,v)
        u.reg_write(UC_ARM64_REG_X30,0x108000);u.reg_write(UC_ARM64_REG_SP,0x210000)
        u.emu_start(0x100000,0x108000,count=10000)
        result=u.reg_read(UC_ARM64_REG_X0)&0xffffffff;result=result-(1<<32) if result&0x80000000 else result
        actual=(result,)+struct.unpack('<QQQ',u.mem_read(0x201000,24)) if result==1 else (result,)
        assert actual==expected,(hex(a),actual,expected)
        receipts.append({'address':a,'regions':rs,'actual':actual})
    (out/'result.json').write_text(json.dumps({'result':'PASS','scope':'compiled free-range AArch64 code, not device SDK execution','cases':receipts},indent=2))
    print(json.dumps({'result':'PASS','cases':len(receipts),'evidence':str(out/'result.json')}))
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('output',type=Path);ap.add_argument('--sdk',type=Path,required=True)
    a=ap.parse_args();main(a.output,a.sdk)
