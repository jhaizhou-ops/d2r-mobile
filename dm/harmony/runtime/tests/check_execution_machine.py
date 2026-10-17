"""CPU-only checks of the ACTUAL compiled gate/leaf machine code with Unicorn.
This is not OHOS execution evidence. Inputs stay in external artifact directories.
"""
import argparse,json,random,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from build_execution import elf_sections,sha
from rewrite_harmony import MD
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
from capstone.arm64_const import ARM64_OP_MEM
LOAD=0x10000000;STACK=0x80000000;DATA=0x90000000;STOP=0x70000000
X=[UC_ARM64_REG_X0+i for i in range(29)]+[UC_ARM64_REG_X29,UC_ARM64_REG_X30]
Q=[UC_ARM64_REG_Q0+i for i in range(32)]
def symbols(raw,sections):
    st=sections['.symtab'];ss=sections['.strtab'];strings=raw[ss[4]:ss[4]+ss[5]];out={}
    for o in range(st[4],st[4]+st[5],24):
        name,info,other,sec,value,size=struct.unpack_from('<IBBHQQ',raw,o)
        if name:out[strings[name:strings.index(0,name)].decode()]=value+LOAD
    return out
def create(raw=None):
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    u.mem_map(STACK,0x40000);u.mem_map(DATA,0x40000);u.mem_map(STOP,0x1000)
    if raw:
        ph,sections,sh=elf_sections(raw);loads=[p for p in ph if p[0]==1]
        end=max(p[3]+p[6] for p in loads);u.mem_map(LOAD,(end+4095)&~4095)
        for p in loads:u.mem_write(LOAD+p[3],raw[p[2]:p[2]+p[5]])
        for s in sh:
            if s[1]==4:
                for p in range(s[4],s[4]+s[5],24):
                    a,info,addend=struct.unpack_from('<QQq',raw,p)
                    if info&0xffffffff==1027:u.mem_write(LOAD+a,struct.pack('<Q',LOAD+addend))
    return u
def put(u,regs,simd,sp,flags):
    for r,v in zip(X,regs):u.reg_write(r,v)
    for r,v in zip(Q,simd):u.reg_write(r,v)
    u.reg_write(UC_ARM64_REG_SP,sp)
    for reg,v in zip((UC_ARM64_REG_NZCV,UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR),flags):u.reg_write(reg,v)
def get(u):
    return ([u.reg_read(r) for r in X],[u.reg_read(r) for r in Q],u.reg_read(UC_ARM64_REG_SP),
        [u.reg_read(r) for r in (UC_ARM64_REG_NZCV,UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR)])
def frame_bytes(regs,simd,sp,flags):
    return struct.pack('<32Q',*regs,sp)+b''.join(v.to_bytes(16,'little') for v in simd)+struct.pack('<8Q',*flags,0,0,0,0,0)
def run(elf,originals,out):
    raw=elf.read_bytes();ph,sects,_=elf_sections(raw);syms=symbols(raw,sects);u=create(raw);orig=create();rng=random.Random(20261009)
    for name in ('main','sdk'):
        b=syms['xblr_'+name+'_base'];data=(originals/f'{name}.original.image.bin').read_bytes()
        orig.mem_map(b,(len(data)+4095)&~4095);orig.mem_write(b,data)
    metas=[]
    for name,a in syms.items():
        if name.endswith('_meta') and name.startswith(('xblr_main_s','xblr_sdk_s')):
            v=struct.unpack('<5Q4I',bytes(u.mem_read(a,56)));metas.append((name,a,v))
    rewrite=json.loads((elf.parent.parent/'generated/rewrite-report.json').read_text())
    folds={syms['xblr_'+r['name']+'_base']+w['start']:(r['name'],w) for r in rewrite['modules'] for w in r.get('hash_loops',[])}
    counts={};cases=[];memory=bytes(rng.getrandbits(8) for _ in range(0x40000));frame=STACK+0x1000
    for label,ma,m in metas:
        fn,pc,resume,taken,target,kind,arg,extra,_=m
        if kind!=0 or not fn:continue
        if pc in folds:continue # entire hash loops are compared below, not one instruction
        ins=list(MD.disasm(bytes(orig.mem_read(pc,4)),pc))[0]
        if ins.mnemonic in ('ldxr','ldaxr','ldxp','ldaxp'):continue # separately exercised as whole windows
        if counts.get(ins.mnemonic,0)>=4:continue
        regs=[rng.getrandbits(64) for _ in range(31)];simd=[rng.getrandbits(128) for _ in range(32)];sp=STACK+0x30000
        flags=[rng.randrange(16)<<28,0,0]
        for op in ins.operands:
            if op.type==ARM64_OP_MEM:
                base=ins.reg_name(op.mem.base) or '';index=ins.reg_name(op.mem.index) or ''
                if base.startswith(('x','w')) and base[1:].isdigit():regs[int(base[1:])]=DATA+0x20000
                if base=='fp':regs[29]=DATA+0x20000
                if base=='lr':regs[30]=DATA+0x20000
                if index.startswith(('x','w')) and index[1:].isdigit():regs[int(index[1:])]=0
        for machine in (u,orig):machine.mem_write(DATA,memory);machine.mem_write(STACK,bytes(0x40000))
        put(orig,regs,simd,sp,flags)
        orig.emu_start(pc,pc+4,count=1)
        expected=get(orig)
        u.mem_write(frame,frame_bytes(regs,simd,sp,flags));host18=0x1855555518
        hregs=[rng.getrandbits(64) for _ in range(31)];hregs[0]=frame;hregs[18]=host18;hregs[30]=STOP
        put(u,hregs,[rng.getrandbits(128) for _ in range(32)],STACK+0x8000,[0,0,0])
        u.emu_start(fn,STOP,count=3000)
        assert u.reg_read(UC_ARM64_REG_PC)==STOP,(label,'helper did not return')
        f=bytes(u.mem_read(frame,832));actualregs=list(struct.unpack_from('<31Q',f));actualsp=struct.unpack_from('<Q',f,248)[0]
        actualsimd=[int.from_bytes(f[256+16*i:272+16*i],'little') for i in range(32)];actualflags=list(struct.unpack_from('<3Q',f,768))
        assert (actualregs,actualsimd,actualsp,actualflags)==expected,(label,ins.mnemonic,ins.op_str,'architectural mismatch')
        assert u.reg_read(X[18])==host18,(label,'host x18 changed')
        assert bytes(u.mem_read(DATA,0x40000))==bytes(orig.mem_read(DATA,0x40000)),(label,'memory mismatch')
        # Guest stack area excludes host helper/gate frames.
        assert bytes(u.mem_read(STACK+0x2ff00,0x10100))==bytes(orig.mem_read(STACK+0x2ff00,0x10100)),(label,'guest stack mismatch')
        counts[ins.mnemonic]=counts.get(ins.mnemonic,0)+1;cases.append({'site':label,'instruction':ins.mnemonic})
    atomic_cases=[]
    for label,ma,m in metas:
        fn,pc,resume,taken,target,kind,arg,extra,_=m
        if kind!=0 or not fn:continue
        first=list(MD.disasm(bytes(orig.mem_read(pc,4)),pc))[0]
        if first.mnemonic not in ('ldxr','ldaxr','ldxp','ldaxp'):continue
        body=[]
        for ins in MD.disasm(bytes(orig.mem_read(pc,256)),pc):
            body.append(ins)
            if ins.mnemonic in ('stxr','stlxr','stxp','stlxp'):break
        end=body[-1].address+4
        for variant in (0,1):
            regs=[0]*31 if not variant else [rng.getrandbits(48) for _ in range(31)]
            simd=[rng.getrandbits(128) for _ in range(32)];sp=STACK+0x30000;flags=[0,0,0]
            for ins in body:
                for op in ins.operands:
                    if op.type==ARM64_OP_MEM:
                        base=ins.reg_name(op.mem.base) or ''
                        if base.startswith('x') and base[1:].isdigit():regs[int(base[1:])]=DATA+0x20000+int(base[1:])*256
            for machine in (u,orig):machine.mem_write(DATA,bytes(0x40000));machine.mem_write(STACK,bytes(0x40000))
            put(orig,regs,simd,sp,flags)
            def original_stop(machine,address,size,user):
                if address<pc or address>=end:machine.emu_stop()
            h=orig.hook_add(UC_HOOK_CODE,original_stop);orig.emu_start(pc,STOP,count=256);orig.hook_del(h)
            expected=get(orig);expectedpc=orig.reg_read(UC_ARM64_REG_PC)
            assert expectedpc<pc or expectedpc>=end,(label,'original window failed to exit')
            buf=bytearray(frame_bytes(regs,simd,sp,flags));struct.pack_into('<Q',buf,792,resume);u.mem_write(frame,bytes(buf))
            hregs=[rng.getrandbits(64) for _ in range(31)];hregs[0]=frame;hregs[18]=0x18181818;hregs[30]=STOP
            put(u,hregs,[0]*32,STACK+0x8000,[0,0,0]);u.emu_start(fn,STOP,count=3000)
            assert u.reg_read(UC_ARM64_REG_PC)==STOP,(label,'atomic helper failed to return')
            f=bytes(u.mem_read(frame,832));actualregs=list(struct.unpack_from('<31Q',f));actualsp=struct.unpack_from('<Q',f,248)[0]
            actualsimd=[int.from_bytes(f[256+16*i:272+16*i],'little') for i in range(32)];actualflags=list(struct.unpack_from('<3Q',f,768))
            assert (actualregs,actualsimd,actualsp,actualflags)==expected,(label,variant,'atomic architectural mismatch')
            cont=struct.unpack_from('<Q',f,792)[0];branch=list(MD.disasm(bytes(u.mem_read(cont+4,4)),cont+4))[0]
            assert branch.mnemonic=='b' and branch.operands[0].imm==expectedpc,(label,'atomic edge mismatch')
            assert bytes(u.mem_read(DATA,0x40000))==bytes(orig.mem_read(DATA,0x40000)),(label,'atomic memory mismatch')
            assert u.reg_read(X[18])==0x18181818,(label,'atomic host x18 changed')
            atomic_cases.append({'site':label,'variant':variant,'instructions':len(body)})
    hash_cases=[]
    for label,ma,m in metas:
        fn,pc,resume,taken,target,kind,arg,extra,_=m
        if pc not in folds:continue
        module,w=folds[pc];base=syms['xblr_'+module+'_base'];end=base+w['end']
        for variant in range(24):
            # Exercise complete 16-round loops and valid partial-loop entries.
            index=(0,7,15)[variant%3]
            regs=[rng.getrandbits(64) for _ in range(31)]
            simd=[rng.getrandbits(128) for _ in range(32)];sp=STACK+0x30000
            flags=[rng.randrange(16)<<28,0x400000,0x08000000]
            regs[8]=base+0x1e11f50;regs[11]=base+0x1e11f90;regs[12]=base+0x1e12090
            block=DATA+0x20000
            if w['start'] in (0x1b6deb4,0x1b6e000):regs[18]=block
            elif w['start']==0x1b6df1c:regs[3]=block
            elif w['start']==0x1b6df90:regs[4]=block
            regs[w['counter']]=index
            payload=bytes(rng.getrandbits(8) for _ in range(64))
            for machine in (u,orig):machine.mem_write(DATA,bytes(0x40000));machine.mem_write(block,payload)
            put(orig,regs,simd,sp,flags);orig.emu_start(pc,end,count=2000)
            assert orig.reg_read(UC_ARM64_REG_PC)==end,(label,'original hash loop did not terminate')
            expected=get(orig)
            buf=bytearray(frame_bytes(regs,simd,sp,flags));struct.pack_into('<Q',buf,792,resume);u.mem_write(frame,bytes(buf))
            hregs=[rng.getrandbits(64) for _ in range(31)];hregs[0]=frame;hregs[18]=0x18181818;hregs[30]=STOP
            put(u,hregs,[0]*32,STACK+0x8000,[0,0,0]);u.emu_start(fn,STOP,count=3000)
            assert u.reg_read(UC_ARM64_REG_PC)==STOP,(label,'hash helper did not return')
            f=bytes(u.mem_read(frame,832));actualregs=list(struct.unpack_from('<31Q',f));actualsp=struct.unpack_from('<Q',f,248)[0]
            actualsimd=[int.from_bytes(f[256+16*i:272+16*i],'little') for i in range(32)];actualflags=list(struct.unpack_from('<3Q',f,768))
            assert (actualregs,actualsimd,actualsp,actualflags)==expected,(label,variant,'hash architectural mismatch')
            assert bytes(u.mem_read(DATA,0x40000))==bytes(orig.mem_read(DATA,0x40000)),(label,'hash input memory changed')
            assert struct.unpack_from('<Q',f,792)[0]==resume and u.reg_read(X[18])==0x18181818
            assert [u.reg_read(X[i]) for i in range(19,31)]==hregs[19:31],(label,'host callee register changed')
            hash_cases.append({'site':label,'index':index,'variant':variant,'original_instructions':(16-index)*w['instructions'],'gates_after_folding':1})
    assert len(hash_cases)==len(folds)*24
    # The actual gate must survive arbitrary dispatcher clobber of all GPR/SIMD
    # and status fields, and restore the guest SP through the static tail.
    tls_cases=[row for row in metas if row[2][5]==1][:8];gate_results=[]
    for label,ma,m in tls_cases:
        fn,pc,resume,taken,target,kind,arg,extra,_=m
        regs=[rng.getrandbits(64) for _ in range(31)];regs[18]=0x18181818;simd=[rng.getrandbits(128) for _ in range(32)]
        sp=STACK+0x30000;flags=[0xa0000000,0x400000,0x08000000];tls=0x123456780000
        put(u,regs,simd,sp,flags);dispatch=syms['xblr_instruction_dispatch']
        def hook(machine,address,size,user):
            if address!=dispatch:return
            f=machine.reg_read(X[0]);lr=machine.reg_read(X[30]);buf=bytearray(machine.mem_read(f,832))
            assert struct.unpack_from('<Q',buf,248)[0]==sp
            struct.pack_into('<Q',buf,18*8,0xfeedface);struct.pack_into('<Q',buf,arg*8,tls)
            struct.pack_into('<Q',buf,792,resume);machine.mem_write(f,bytes(buf))
            for i,r in enumerate(X):
                if i!=18:machine.reg_write(r,0xbad00000+i)
            for i,r in enumerate(Q):machine.reg_write(r,0xbad10000+i)
            for r in (UC_ARM64_REG_NZCV,UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR):machine.reg_write(r,0)
            machine.reg_write(UC_ARM64_REG_PC,lr)
        h=u.hook_add(UC_HOOK_CODE,hook)
        u.emu_start(pc,pc+4,count=2000);u.hook_del(h)
        want=regs.copy();want[arg]=tls
        actual=get(u)
        assert actual==(want,simd,sp,flags),(label,'full gate state mismatch',
            [(i,hex(a),hex(b)) for i,(a,b) in enumerate(zip(actual[0],want)) if a!=b],actual[2:],(sp,flags))
        gate_results.append(label)
    result={'result':'PASS','scope':'actual ARM64 compiled helper and full-state gate under Unicorn, not Harmony device execution',
        'elf_sha256':sha(elf),'test_script_sha256':sha(__file__),'plain_original_vs_rewritten_cases':len(cases),
        'instruction_families':counts,'exclusive_window_cases':atomic_cases,'hash_loop_cases':hash_cases,
        'full_gate_clobber_cases':len(gate_results),'gate_sites':gate_results}
    out.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('elf',type=Path);ap.add_argument('originals',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args();run(a.elf,a.originals,a.output)
