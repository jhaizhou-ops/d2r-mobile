"""Offline original AArch64 -> OHOS, all generated game bytes stay outside Git.

Every intercepted instruction enters a full architectural-state gate. Plain x18
instructions execute as a statically compiled leaf operation on the saved frame.
No host TLS layout, red zone, JIT, host x18 or reserved x28 is used.
"""
import argparse, hashlib, json, re, struct
from collections import Counter
from itertools import combinations
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

MD=Cs(CS_ARCH_ARM64,CS_MODE_ARM); MD.detail=True
KINDS={'plain':0,'tls_read':1,'tls_write':2,'tlr_read':3,'counter':4,'svc':5,
       'adr':6,'cbz':7,'cbnz':8,'tbz':9,'tbnz':10,'br':11}
# r32 MD5 transform's four original fixed-count round loops. This folds original
# machine operations, not the algorithm/result: no hash HLE or skipped check.
HASH_LOOPS=((0x1b6deb4,0x1b6df14,17),(0x1b6df1c,0x1b6df88,1),
            (0x1b6df90,0x1b6dff8,3),(0x1b6e000,0x1b6e068,17))
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def dis(w,a):
    xs=list(MD.disasm(struct.pack('<I',w),a));return xs[0] if xs else None
def rewrite_regs(ins,mapping):
    want=re.sub(r'\b([xw])(\d+)\b',lambda m:m[1]+str(mapping.get(int(m[2]),int(m[2]))),ins.op_str)
    want=re.sub(r'\b(w?sp)\b',lambda m:('w' if m[1]=='wsp' else 'x')+str(mapping[31]),want) if 31 in mapping else want
    if ins.mnemonic=='mov' and 31 in mapping:
        r=re.fullmatch(r'x(\d+), x(\d+)',want)
        if r:return 0xaa0003e0 | (int(r[2])<<16) | int(r[1])
    word=int.from_bytes(ins.bytes,'little'); variants=[word]
    for old,new in mapping.items():
        nex=[]
        for w in variants:
            ps=[p for p in (0,5,10,16) if (w>>p)&31==old]
            for n in range(len(ps)+1):
                for ss in combinations(ps,n):
                    nw=w
                    for p in ss:nw=(nw&~(31<<p))|(new<<p)
                    nex.append(nw)
        variants=nex
    for w in variants:
        x=dis(w,ins.address)
        if x and x.mnemonic==ins.mnemonic and x.op_str==want:return w
    raise ValueError(f'cannot substitute {ins.address:x}: {ins.mnemonic} {ins.op_str} -> {want}')
def classify(w,a):
    masked=w&0xffffffe0
    for value,kind in ((0xd53bd040,'tls_read'),(0xd51bd040,'tls_write'),(0xd53bd060,'tlr_read'),
                       (0xd53be020,'counter'),(0xd53be040,'counter')):
        if masked==value:return kind,w&31
    if w&0xffe0001f==0xd4000001:return 'svc',(w>>5)&0xffff
    if not any((w>>p)&31==18 for p in (0,5,10,16)):return None
    i=dis(w,a)
    if i and re.search(r'\b[xw]18\b',i.op_str):return 'plain',0
    return None
def op_body(ins,label,window=None,tail_targets=None):
    # Use only caller-saved scratch registers, while preserving the C ABI with a
    # full callee-saved spill. The frame's SIMD and status state is loaded before
    # the ONE original operation and written back immediately afterward.
    window=window or [ins];regs=set()
    for item in window:
        regs|={int(x) for x in re.findall(r'\b[xw](\d+)\b',item.op_str)}
        rr,rw=item.regs_access()
        for r in rr+rw:
            n=item.reg_name(r)
            if n=='lr':regs.add(30)
            elif n=='fp':regs.add(29)
            elif re.fullmatch('[xw][0-9]+',n):regs.add(int(n[1:]))
    scratch=[r for r in range(0,16) if r not in regs]
    assert len(scratch)>=4,f'not enough safe scratch registers at {ins.address:x}'
    fp,tmp,spreg,edge=scratch[:4];mapping={18:tmp}
    has_sp=any(re.search(r'\bw?sp\b',item.op_str) for item in window)
    for item in window:
        if re.search(r'\bw?sp\b',item.op_str):
            for imm in re.findall(r'#(-(?:0x)?[0-9a-f]+)',item.op_str):
                assert int(imm,0)>=-192,f'guest SP access overlaps gate frame at {item.address:x}'
    if has_sp:mapping[31]=spreg
    if len(window)==1 and ins.mnemonic in ('ldxr','ldaxr','ldxp','ldaxp','stxr','stlxr','stxp','stlxp'):
        raise ValueError(f'exclusive operation needs a whole-window rewrite: {ins.address:x}')
    lines=[f'{label}:','sub sp, sp, #256']
    for r in range(19,31,2):lines.append(f'stp x{r}, x{r+1}, [sp, #{(r-19)*8}]')
    for r in range(8,16,2):lines.append(f'stp q{r}, q{r+1}, [sp, #{96+(r-8)*16}]')
    lines.append(f'mov x{fp}, x0')
    # q loads precede flags, and none of the scaffolding changes NZCV.
    for r in range(0,32,2):lines.append(f'ldp q{r}, q{r+1}, [x{fp}, #{256+r*16}]')
    for off,name in ((768,'nzcv'),(776,'fpcr'),(784,'fpsr')):
        lines.extend([f'ldr x{tmp}, [x{fp}, #{off}]',f'msr {name}, x{tmp}'])
    for r in sorted(regs):lines.append(f'ldr x{mapping.get(r,r)}, [x{fp}, #{r*8}]')
    if has_sp:lines.append(f'ldr x{spreg}, [x{fp}, #248]')
    addrs={item.address for item in window};edges={}
    for item in window:
        lines.append(f'{label}_at_{item.address:x}:')
        if item.mnemonic in ('b','cbz','cbnz','tbz','tbnz') or item.mnemonic.startswith('b.'):
            target=item.operands[-1].imm
            dest=f'{label}_at_{target:x}' if target in addrs else f'{label}_exit_{target:x}'
            if target not in addrs:edges[target]=dest
            operands=item.op_str.rsplit(',',1)[0]+', '+dest if ',' in item.op_str else dest
            operands=re.sub(r'\b([xw])18\b',lambda m:m[1]+str(tmp),operands)
            lines.append(item.mnemonic+' '+operands)
        else:lines.append(f'.inst 0x{rewrite_regs(item,mapping):08x}')
    if edges:
        lines.append(f'b {label}_writeback')
        for target,dest in edges.items():
            tail=tail_targets[target]
            lines += [f'{dest}:',f'adrp x{edge}, {tail}',f'add x{edge}, x{edge}, :lo12:{tail}',f'str x{edge}, [x{fp}, #792]',f'b {label}_writeback']
        lines.append(f'{label}_writeback:')
    for r in sorted(regs):lines.append(f'str x{mapping.get(r,r)}, [x{fp}, #{r*8}]')
    if has_sp:lines.append(f'str x{spreg}, [x{fp}, #248]')
    for off,name in ((768,'nzcv'),(776,'fpcr'),(784,'fpsr')):
        lines.extend([f'mrs x{tmp}, {name}',f'str x{tmp}, [x{fp}, #{off}]'])
    for r in range(0,32,2):lines.append(f'stp q{r}, q{r+1}, [x{fp}, #{256+r*16}]')
    for r in range(8,16,2):lines.append(f'ldp q{r}, q{r+1}, [sp, #{96+(r-8)*16}]')
    for r in range(19,31,2):lines.append(f'ldp x{r}, x{r+1}, [sp, #{(r-19)*8}]')
    lines+=['add sp, sp, #256','ret'];return lines
def check_window_entries(raw,end,windows):
    covered={i.address for body in windows.values() for i in body}
    interiors=covered-set(windows)
    for a in range(0,end,4):
        w=struct.unpack_from('<I',raw,a)[0];target=None
        if w&0x7c000000==0x14000000:
            imm=w&0x3ffffff;target=a+((imm-(1<<26) if imm&(1<<25) else imm)<<2)
        elif w&0xff000010==0x54000000 or w&0x7e000000==0x34000000:
            imm=(w>>5)&0x7ffff;target=a+((imm-(1<<19) if imm&(1<<18) else imm)<<2)
        elif w&0x7e000000==0x36000000:
            imm=(w>>5)&0x3fff;target=a+((imm-(1<<14) if imm&(1<<13) else imm)<<2)
        if target in interiors and a not in covered:raise ValueError(f'branch into folded window {a:x}->{target:x}')

def hash_windows(raw,end,sites,windows):
    covered=set();rows=[]
    for start,stop,counter in HASH_LOOPS:
        body=list(MD.disasm(bytes(raw[start:stop]),start));assert len(body)*4==stop-start
        assert body[-1].mnemonic=='b.ne' and body[-1].operands[0].imm==start
        assert body[-2].mnemonic=='cmp' and body[-2].op_str==f'x{counter}, #0x10'
        assert sum(i.mnemonic=='add' and i.op_str==f'x{counter}, x{counter}, #1' for i in body)==1
        assert all(i.mnemonic not in ('adr','adrp','bl','blr','br','ret','svc','b','cbz','cbnz','tbz','tbnz') and not i.mnemonic.startswith('b.') for i in body[:-1])
        assert not any(i.address in windows for i in body)
        assert any(re.search(r'\b[xw]18\b',i.op_str) for i in body)
        windows[start]=body;covered|={i.address for i in body}
        rows.append({'start':start,'end':stop,'counter':counter,'instructions':len(body),
                     'original_sha256':hashlib.sha256(raw[start:stop]).hexdigest()})
    check_window_entries(raw,end,windows)
    kept=[s for s in sites if s[0] not in covered]
    kept += [(a,int.from_bytes(windows[a][0].bytes,'little'),'hotloop',0) for a,_,_ in HASH_LOOPS]
    return sorted(kept),rows

def exclusive_windows(raw,end,sites):
    """Keep hardware reservations wholly inside a single native leaf operation."""
    changed={s[0] for s in sites};windows={};covered=set()
    for a in range(0,end,4):
        w=struct.unpack_from('<I',raw,a)[0]
        if w&0x3f000000!=0x08000000:continue
        first=dis(w,a)
        if not first or first.mnemonic not in ('ldxr','ldaxr','ldxp','ldaxp'):continue
        body=[first]
        for b in range(a+4,min(a+256,end),4):
            i=dis(struct.unpack_from('<I',raw,b)[0],b)
            if not i or i.mnemonic in ('ldxr','ldaxr','ldxp','ldaxp','ret','blr','bl'):break
            body.append(i)
            if i.mnemonic in ('stxr','stlxr','stxp','stlxp'):
                if any(i.address in changed for i in body):
                    windows[a]=body;covered|={i.address for i in body}
                break
    check_window_entries(raw,end,windows)
    new=[s for s in sites if s[0] not in covered]
    for a,body in windows.items():new.append((a,int.from_bytes(body[0].bytes,'little'),'window',0))
    return sorted(new),windows
def ui_observation_site(name,raw):
    if name!='main':return None
    a=0xb1dc64;w=struct.unpack_from('<I',raw,a)[0]
    assert raw[a-4:a+4].hex()=='ff0306d1fc6f12a9','r32 UI prologue changed'
    return (a,w,'plain',0x5549)

def world_tick_observation_site(name,raw):
    if name!='main':return None
    # Original OBS_TICK is +5d3bc0 (sub sp,#0x140). Leave that SP
    # allocation untouched and observe x0 before +5d3bc4 replays str d8.
    a=0x5d3bc4;w=struct.unpack_from('<I',raw,a)[0]
    assert raw[a-4:a+4].hex()=='ff0305d1e86b00fd','r32 tick prologue changed'
    return (a,w,'plain',0x5754)

def product_cuts(module):
    """Only the existing r32 offline/start-menu behavior; no iOS host-call cuts."""
    if module['name']!='main':return []
    import ast
    source=Path(__file__).resolve().parents[2]/'compat-r32/offline_cuts.py'
    tree=ast.parse(source.read_text(encoding='utf-8'))
    table=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='CUTS' for t in n.targets))
    allowed={0x1979bc,0x5b14dc,0x5f3184,0x1d53e80,0x8019ec}
    rows=[c for c in table[module['build_id'][:32]] if c[0] in allowed]
    assert len(rows)==len(allowed)
    return [{'offset':c[0],'before':c[1],'after':c[2],'reason':c[3],'source_sha256':sha(source)} for c in rows]

def generate(inputs,out):
    mf=json.loads((inputs/'originals-manifest.json').read_text())
    assert mf['result']=='PASS' and mf['product']=='3.3.94012-r32'
    out.mkdir(parents=True,exist_ok=False)
    allasm=['.arch armv8-a'];descs=['#include "xblr_loader.h"'];descrows=[];layout=['SECTIONS {'];reports=[]
    for m in mf['modules']:
        name=m['name'];inp=inputs/(name+'.original.image.bin');assert sha(inp)==m['image_sha256']
        raw=bytearray(inp.read_bytes());text_end=m['segments'][0]['bytes'];sites=[]
        cuts=product_cuts(m)
        for c in cuts:
            a=c['offset'];before=bytes.fromhex(c['before']);after=bytes.fromhex(c['after'])
            assert len(before)==len(after) and raw[a:a+len(before)]==before,(name,hex(a),'r32 cut original mismatch')
            raw[a:a+len(after)]=after
        segment_file=inp
        if cuts:
            segment_file=out/(name+'.harmony.segments.bin');segment_file.write_bytes(raw)
        for a in range(0,text_end,4):
            w=struct.unpack_from('<I',raw,a)[0];kind=classify(w,a)
            if kind:sites.append((a,w,*kind))
        if name=='main':
            # r32 UI observation after the original SP allocation. Observe x1
            # before the callee changes it; replay the original store unchanged.
            # Do not intercept sub sp,#0x180: that SP delta exceeds gate headroom.
            observation=ui_observation_site(name,raw);a=observation[0]
            assert all(s[0]!=a for s in sites)
            sites.append(observation)
            tick_observation=world_tick_observation_site(name,raw);a=tick_observation[0]
            assert all(s[0]!=a for s in sites)
            sites.append(tick_observation);sites.sort()
        sites,windows=exclusive_windows(raw,text_end,sites)
        hot_loops=[]
        if name=='main':sites,hot_loops=hash_windows(raw,text_end,sites,windows)
        stats=Counter();tails=[];stubs=[];ops=[];meta=[];ranges=[];opcache={}
        # EH boundaries are used only to prove static br-x18 jump table bounds,
        # matching the r32 rewriter's recognized ADR + scaled index idiom.
        for idx,(a,w,kind,arg) in enumerate(sites):
            ins=dis(w,a);label=f'xblr_{name}_s{idx}';original=f'xblr_{name}_base + {a}'
            target=0;extra=0;fn='0';resume=label+'_resume';taken='0';base=0;count=0
            resume_offset=a+4
            if kind in ('window','hotloop'):
                body=windows[a];resume_offset=body[-1].address+4;tail_targets={}
                for item in body:
                    if item.mnemonic in ('b','cbz','cbnz','tbz','tbnz') or item.mnemonic.startswith('b.'):
                        t=item.operands[-1].imm
                        if not a<=t<resume_offset and t not in tail_targets:
                            tl=label+f'_out_{t:x}';tail_targets[t]=tl
                            tails += [f'.global {tl}',f'.hidden {tl}',f'{tl}:','ldp x16, x17, [sp], #16',f'b xblr_{name}_base + {t}']
                fn=label+'_op';ops.append((fn,op_body(ins,fn,body,tail_targets)))
                stats['exclusive_window' if kind=='window' else 'hash_loop']+=1;kind='plain'
            if kind=='plain':
                mn=ins.mnemonic
                if mn in ('adr','adrp'):kind='adr';target=ins.operands[1].imm
                elif mn in ('cbz','cbnz','tbz','tbnz'):
                    kind=mn;target=ins.operands[-1].imm;taken=label+'_taken'
                    arg=32 if ins.op_str.startswith('w') else 64
                    if mn.startswith('tb'):extra=ins.operands[1].imm
                    tails += [f'{taken}:','ldp x16, x17, [sp], #16',f'b xblr_{name}_base + {target}']
                elif mn=='br':
                    kind='br';ctx=[dis(struct.unpack_from('<I',raw,o)[0],o) for o in range(max(0,a-32),a,4)]
                    for p in reversed(ctx):
                        if p and p.mnemonic=='adr' and p.op_str.startswith('x18,'):base=p.operands[1].imm;break
                    for p in reversed(ctx):
                        if p and p.mnemonic=='add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                            r=ins.reg_name(p.operands[2].reg)
                            for q in reversed(ctx):
                                if q and q.op_str.startswith(('x'+r[1:]+',','w'+r[1:]+',')):
                                    if q.mnemonic in ('ldrb','ldrsb'):count=256;break
                                    if q.mnemonic in ('ldrh','ldrsh'):count=65536;break
                            break
                    assert base and count,f'unrecognized br x18 {name}+{a:x}'
                    assert base+4*count<=text_end
                    target=base;extra=count;taken=label+'_pads'
                    tails += [f'{taken}:']
                    for j in range(count):tails += ['ldp x16, x17, [sp], #16',f'b xblr_{name}_base + {base+4*j}']
                    ranges.append({'site':a,'base':base,'count':count})
                elif mn in ('blr','ret','b','bl') or (mn.startswith('ldr') and '[' not in ins.op_str):
                    raise ValueError(f'unsupported PC-dependent x18 {name}+{a:x} {mn}')
                elif fn=='0':
                    if w in opcache:fn=opcache[w]
                    else:
                        fn=label+'_op';ops.append((fn,op_body(ins,fn)));opcache[w]=fn
            stats[kind]+=1
            stubs += [f'.global {label}',f'.hidden {label}',f'{label}:','sub sp, sp, #256','stp x16, x30, [sp]',
                      f'adrp x16, {label}_meta',f'add x16, x16, :lo12:{label}_meta','bl xblr_instruction_gate',
                      f'{resume}:','ldp x16, x17, [sp], #16',f'b xblr_{name}_base + {resume_offset}']
            meta += [f'{label}_meta:',f'.xword {fn}',f'.xword {original}',f'.xword {resume}',f'.xword {taken}',
                     f'.xword xblr_{name}_base + {target}' if kind in ('adr','br') else '.xword 0',
                     f'.word {KINDS[kind]}, {arg}, {extra}, 0']
        # Original instruction replaced by a static B relocation in the assembler.
        asm=[f'.section .xblr.{name}.text,"ax",@progbits','.p2align 12',f'.global xblr_{name}_base',f'.hidden xblr_{name}_base',f'xblr_{name}_base:']
        code=bytearray(raw[:text_end]);relocs=[]
        for idx,(a,w,kind,arg) in enumerate(sites):
            struct.pack_into('<I',code,a,0x14000000)
            relocs += [f'.global xblr_{name}_s{idx}',f'.hidden xblr_{name}_s{idx}',
                       f'.reloc xblr_{name}_base+{a}, R_AARCH64_JUMP26, xblr_{name}_s{idx}']
            if a in windows:
                for i in range(1,len(windows[a])):struct.pack_into('<I',code,a+4*i,0xd420e620)
        codefile=out/(name+'.harmony.text.bin');codefile.write_bytes(code)
        asm.append(f'.incbin "{codefile.as_posix()}"');asm+=relocs
        ro=m['segments'][1]['offset'];data=m['segments'][2]['offset'];size=m['image_bytes']
        # 4K module layout is intrinsic to the original NSO; fail at runtime on a
        # host with larger pages until a verified signed remap path is available.
        asm += [f'.section .xblr.{name}.ro,"a",@progbits',f'.incbin "{segment_file.as_posix()}", {ro}, {data-ro}',
                f'.section .xblr.{name}.data,"aw",@progbits',f'.incbin "{segment_file.as_posix()}", {data}, {size-data}']
        allasm+=asm
        gates=[f'.section .xblr.{name}.gates,"ax",@progbits']+stubs+tails
        gates+=['.section .data.rel.ro.xblr_sites,"aw",@progbits','.p2align 3']+meta+['.section .note.GNU-stack,"",@progbits']
        (out/f'harmony_{name}_gates.S').write_text('\n'.join(gates)+'\n')
        for chunk in range(0,len(ops),256):
            lines=[f'.section .xblr.{name}.gates,"ax",@progbits']
            for fn,body in ops[chunk:chunk+256]:lines += [f'.global {fn}',f'.hidden {fn}']+body
            lines+=['.section .note.GNU-stack,"",@progbits']
            (out/f'harmony_{name}_ops_{chunk//256:03d}.S').write_text('\n'.join(lines)+'\n')
        layout += [f'.xblr_{name}_text ALIGN(0x1000) : {{ KEEP(*(.xblr.{name}.text)) }}',
                   f'.xblr_{name}_ro (xblr_{name}_base + {ro}) : {{ KEEP(*(.xblr.{name}.ro)) }}',
                   f'.xblr_{name}_data (xblr_{name}_base + {data}) : {{ KEEP(*(.xblr.{name}.data)) }}',
                   f'.xblr_{name}_gates ALIGN(0x1000) : {{ KEEP(*(.xblr.{name}.gates)) }}']
        descs += [f'extern const uint8_t xblr_{name}_base[];']
        ba=lambda k:','.join('0x'+m[k][i:i+2] for i in range(0,len(m[k]),2))
        descrows += ['{.name="'+name+'",.base=xblr_'+name+f'_base,.size={size},.text_bytes={text_end},.ro_offset={ro},.ro_bytes={m["segments"][1]["bytes"]},.data_offset={data},.data_bytes={m["segments"][2]["bytes"]},.bss_bytes={m["bss_bytes"]},.source_kind=XBLR_IMAGE_HARMONY_REWRITTEN,.readonly=0,.source_sha256={{'+ba('sha256')+'},.build_id={'+ba('build_id')+'}}']
        reports += [{'name':name,'original_image_sha256':sha(inp),'sites':len(sites),'unique_leaf_operations':len(ops),'kinds':dict(stats),'jump_ranges':ranges,'product_cuts':cuts,'hash_loops':hot_loops}]
    allasm+=['.section .note.GNU-stack,"",@progbits']
    (out/'harmony_images.S').write_text('\n'.join(allasm)+'\n')
    descs += ['const xblr_image_desc xblr_execution_images[2]={'+',\n'.join(descrows)+'};']
    (out/'harmony_images.c').write_text('\n'.join(descs)+'\n')
    (out/'harmony_layout.ld').write_text('\n'.join(layout+['}','INSERT AFTER .text;'])+'\n')
    (out/'rewrite-report.json').write_text(json.dumps({'product':mf['product'],'modules':reports,'cuts_applied':sum(len(r['product_cuts']) for r in reports),'apple_tls_used':False,'requires_host_page_bytes':4096},indent=2))
    return reports
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--originals',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
    root=Path(__file__).resolve().parents[3]
    if a.output.resolve().is_relative_to(root) or not str(a.output.resolve()).isascii():raise SystemExit('output must be external ASCII path')
    print(json.dumps(generate(a.originals.resolve(),a.output.resolve()),indent=2))
