"""物品门生产 AST 回归；宿主决策为显式替身，不运行游戏。
Production-AST item-gate regression; host decisions are explicit doubles, not a game run.
"""
import ast
import hashlib
import os
import sys
from pathlib import Path
import struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r

source=Path('dm/tools/patch_nso.py');text=source.read_text();tree=ast.parse(text)
original=Path(os.environ.get('DM_MAIN_ORIGINAL','dm/host/tests/build/main.orig.bin')).read_bytes()
assert hashlib.sha256(original).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
contracts={'pc_item_left':(0x9d,0x3ca47c),'pc_item_right':(0x9e,0x3ca58c),'pc_item_hold':(0x9f,0x3b8648),'pc_npc_click':(0xa0,0x3db0b4),'pc_held_draw':(0xa1,0xabd428)}
names={'B','STP_PRE','LDP_POST','MRS_TPIDRRO','ADRP','ADDI','LDRX','ADDR','BLR','BR','emit_gate_call','emit_slot_addr','emit_key_addr'}
functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in names]
allcontracts=dict(contracts,pc_item_tooltip=(0xa2,0x3cc97c),pc_grid_hover=(0xa3,0xabcca4))
blocks={tag:next(n for n in tree.body if isinstance(n,ast.If) and any(isinstance(q,ast.Constant) and q.value==tag for q in ast.walk(n))) for tag in allcontracts}
write=next(n for n in tree.body if isinstance(n,ast.Expr) and isinstance(n.value,ast.Call) and isinstance(n.value.func,ast.Attribute) and n.value.func.attr=='makedirs')
assert all(n.end_lineno<write.lineno for n in blocks.values()),'Gates must be emitted before output serialization'
patch=bytearray();gates=[];changes=[];patch_off=0x3400000;key=0x6000000
context={'struct':struct,'IMG':original,'RULE_RUN':True,'PRIVATE_GATES':True,'TEXT_END':0x1b80000,'kinds':{},'windows':[],'gates':gates,'cuts':[],'KEY_PAGE_OFF':key,'KEY_INDEX':{'KGATE':0},'fix':[],'patch':patch}
context.update(here=lambda:patch_off+len(patch),emit=lambda word:patch.extend(struct.pack('<I',word)),stat=lambda *_:None,put_branch=lambda at,to:changes.append((at,to)))
exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),context)
for node in blocks.values():exec(compile(ast.Module(body=[node],type_ignores=[]),str(source),'exec'),context)
image=bytearray(original);image.extend(bytes(patch_off+len(patch)-len(image)));image[patch_off:]=patch
for at,to in changes:struct.pack_into('<I',image,at,context['B'](at,to))
sys.path.insert(0,str(source.parent.parent/'verify'))
from pc_items_struct import verify
header={'PATCH_OFF':patch_off,'KEY_PAGE_OFF':key,'KEY_KGATE_IDX':0}
rows={ret:(kind,number,site) for ret,kind,number,site in gates}
cuts=[(off,bytes.fromhex(before),bytes.fromhex(after),why) for off,before,after,why,_ in context['cuts']]
verified=verify('main',original,image,header,rows,cuts)
assert len(verified)==7 and not verify('sdk',original,image,header,rows,cuts)
mutants=0
for tag,(number,site) in allcontracts.items():
 start=next(to for at,to in changes if at==site)
 following=min([to for at,to in changes if to>start]+[len(image)])
 for at in range(start,following,4):
  image[at]^=1
  try:verify('main',original,image,header,rows,cuts)
  except ValueError:mutants+=1
  else:raise AssertionError(f'Uncaught stub mutation {number:x} {at:x}')
  finally:image[at]^=1
print(f'PASS structural verifier: seven real stubs, {mutants} single-word mutations rejected')
regs=[getattr(r,f'UC_ARM64_REG_X{i}') for i in range(31)];checks=0
for tag,(number,site) in contracts.items():
 ret=next(row[0] for row in gates if row[1:]==('svc',number,site))
 for slide in (0,0x100000000):
  for handled in (0,1):
   for condition in (0,1):
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(slide,(len(image)+4095)&~4095);u.mem_write(slide,bytes(image))
    for page in (key+slide,0x7000000,0x8000000,0x9000000,0x9100000):u.mem_map(page,0x10000)
    u.mem_write(key+slide,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
    values=[0x1110000+i*16 for i in range(31)];values[0]=condition;values[8]=0x9100000
    for register,value in zip(regs,values):u.reg_write(register,value)
    stack=0x7008000;flags=0xa0000000|(condition<<30);u.reg_write(r.UC_ARM64_REG_SP,stack);u.reg_write(r.UC_ARM64_REG_NZCV,flags);calls=[]
    def host(cpu,at,size,data):
     if at==0x9000000:
      calls.append(at);assert cpu.reg_read(r.UC_ARM64_REG_LR)==slide+ret
      cpu.reg_write(regs[0] if number==0xa1 else regs[8],0x11223344 if number==0xa1 and handled else values[0] if number==0xa1 else handled)
      cpu.reg_write(r.UC_ARM64_REG_PC,slide+ret)
    u.hook_add(UC_HOOK_CODE,host)
    if number==0x9d:target=slide+(0x3ca590 if handled else 0x3ca5a0 if condition else site+4)
    elif number==0x9e:target=slide+site+4 if handled else values[8]
    elif number==0x9f:target=slide+(0x3b8664 if handled else site+4)
    elif number==0xa0:target=slide+(0x3db198 if handled else 0x3db2b4 if condition else site+4)
    else:target=slide+0x2ab480
    u.emu_start(slide+site,target,count=100)
    expected=values[:]
    if number==0x9f and not handled:expected[8]=slide+0x2d12000
    if number==0x9e and not handled or number==0xa1:expected[30]=slide+site+4
    if number==0xa1 and handled:expected[0]=0x11223344
    assert calls==[0x9000000] and [u.reg_read(reg) for reg in regs]==expected,(tag,slide,handled,condition)
    assert u.reg_read(r.UC_ARM64_REG_SP)==stack and u.reg_read(r.UC_ARM64_REG_NZCV)==flags
    checks+=1
print('PASS',checks,'production AST cases: ASLR, destinations, GPR/SP/NZCV and original LR; host dispatcher doubled')
