"""A2 物品 getter 门的寄存器回归；宿主分派为显式替身。
A2 item-getter gate register regression; host dispatch is an explicit test double.
"""
from pathlib import Path
import os
import ast,hashlib,struct,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r
source=Path('dm/tools/patch_nso.py');original=Path(os.environ.get('DM_MAIN_ORIGINAL','dm/host/tests/build/main.orig.bin')).read_bytes()
assert hashlib.sha256(original).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
site=0x3cc97c;old=0x3cc8b8;regs=[getattr(r,f'UC_ARM64_REG_X{i}') for i in range(31)]
if len(sys.argv)>1:
 p=Path(sys.argv[1]);image=bytearray((p/'main.patched.bin').read_bytes())
 rows=[s.split('\t') for s in (p/'main.gates.tsv').read_text().splitlines() if s and not s.startswith('#')]
 ret=next(int(row[0],16) for row in rows if row[1:]==['svc','162',hex(site)])
 headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0) for line in (p/'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
 key_page=headers['KEY_PAGE_OFF'];key_index=headers['KEY_KGATE_IDX'];mode='producto'
else:
 # 执行相同的生产 AST，不复制其指令。 | Execute the same production AST rather than a replica of its instructions.
 tree=ast.parse(source.read_text());names={'B','STP_PRE','LDP_POST','MRS_TPIDRRO','ADRP','LDRX','ADDR','BLR','emit_gate_call','emit_slot_addr','emit_key_addr'}
 functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in names]
 block=next(n for n in tree.body if isinstance(n,ast.If) and any(isinstance(q,ast.Constant) and q.value=='pc_item_tooltip' for q in ast.walk(n)))
 patch=bytearray();gates=[];changes=[];patch_off=0x3400000;key_page=0x6000000;key_index=0
 env={'struct':struct,'IMG':original,'RULE_RUN':True,'PRIVATE_GATES':True,'TEXT_END':0x1b80000,'kinds':{},'windows':[],'gates':gates,'cuts':changes,'KEY_PAGE_OFF':key_page,'KEY_INDEX':{'KGATE':0},'fix':[],'patch':patch}
 env.update(here=lambda:patch_off+len(patch),emit=lambda word:patch.extend(struct.pack('<I',word)),stat=lambda *_:None)
 def put(at,to):changes.append((at,to))
 env['put_branch']=put
 exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),env)
 exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),env)
 image=bytearray(original);image.extend(bytes(patch_off+len(patch)-len(image)));image[patch_off:]=patch
 for item in changes:
  if len(item)==2:struct.pack_into('<I',image,item[0],env['B'](*item))
 ret=gates[0][0];mode='bloque AST de producción, sin PRODUCT'
print('SHA256 imagen',hashlib.sha256(image).hexdigest(),mode)
checks=0
for f0,allow in ((0,0),(1,0),(1,1),(0,1)):
 for flags in (0,0xa0000000,0xf0000000):
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,bytes(image))
  for page in (0x6000000,0x7000000,0x8000000,0x9000000):
   if page>len(image):u.mem_map(page,0x10000)
  u.mem_write(key_page+key_index*8,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
  values=[0x1110000+i*16 for i in range(31)];values[9]=f0;values[30]=0x3d4fe4
  for reg,value in zip(regs,values):u.reg_write(reg,value)
  sp=0x7008000;u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags);calls=[]
  def hook(cpu,at,size,data):
   if at==0x9000000:
    seen=[cpu.reg_read(reg) for reg in regs];expected=values[:];expected[8]=values[30];expected[30]=ret
    assert seen==expected,'Transporte caller y registros antes del host'
    calls.append(at)
    if allow:cpu.reg_write(regs[9],0)
    cpu.reg_write(r.UC_ARM64_REG_PC,ret)
   elif at in (old,site+4):cpu.emu_stop()
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(site,0x9999999,count=100)
  expected=values[:]
  if allow:expected[9]=0
  assert [u.reg_read(reg) for reg in regs]==expected and u.reg_read(r.UC_ARM64_REG_SP)==sp and u.reg_read(r.UC_ARM64_REG_NZCV)==flags
  assert u.reg_read(r.UC_ARM64_REG_PC)==(old if f0 and not allow else site+4) and calls==[0x9000000]
  checks+=1
print(f'PASS {checks} A2: GPR/SP/LR/NZCV intactos salvo decisión w9; ambos destinos, raw f0=0, caller transportado. Host sink, sin GUI.')
