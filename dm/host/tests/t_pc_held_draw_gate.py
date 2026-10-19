"""GateA1 del producto: sólo XY y LR de BL cambian; host y GPU doblados."""
from pathlib import Path
import hashlib,struct,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r
p=Path(sys.argv[1]);image=(p/'main.patched.bin').read_bytes();rows=[line.split('\t') for line in (p/'main.gates.tsv').read_text().splitlines() if line and not line.startswith('#')]
site,callee=0xabd428,0x2ab480;ret=next(int(row[0],16) for row in rows if row[1:]==['svc','161',hex(site)])
headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0) for line in (p/'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')};regs=[getattr(r,f'UC_ARM64_REG_X{i}') for i in range(31)]
print('SHA256',hashlib.sha256(image).hexdigest())
for pc in (0,1):
 for point in ((2034,922),(2034,-45)):
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,image)
  for page in (0x7000000,0x8000000,0x9000000):u.mem_map(page,0x10000)
  u.mem_write(headers['KEY_PAGE_OFF']+headers['KEY_KGATE_IDX']*8,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
  sp=0x7008000;flags=0xa0000000;u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags)
  values=[0x1110000+i*16 for i in range(31)];values[0]=125|(230<<32)
  for reg,value in zip(regs,values):u.reg_write(reg,value)
  physical=(point[0]&0xffffffff)|((point[1]&0xffffffff)<<32);calls=[];draws=[]
  def hook(cpu,at,size,data):
   if at==0x9000000:
    calls.append(at);assert cpu.reg_read(r.UC_ARM64_REG_LR)==ret
    assert [cpu.reg_read(reg) for reg in regs[:-1]]==values[:-1]
    if pc:cpu.reg_write(regs[0],physical)
    cpu.reg_write(r.UC_ARM64_REG_PC,ret)
   elif at==callee:
    draws.append(cpu.reg_read(regs[0]));assert cpu.reg_read(r.UC_ARM64_REG_LR)==site+4;cpu.reg_write(r.UC_ARM64_REG_PC,site+4)
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(site,site+4,count=100)
  expected=values[:];expected[30]=site+4
  if pc:expected[0]=physical
  assert calls==[0x9000000] and draws==[expected[0]] and [u.reg_read(reg) for reg in regs]==expected
  assert u.reg_read(r.UC_ARM64_REG_SP)==sp and u.reg_read(r.UC_ARM64_REG_NZCV)==flags
  print(f'PASS A1 pc={pc} physical={point}: único BL2ab480, argumentoXY y LR correctos, otrosGPR/SP/NZCV intactos')
print('PASS4 gateproducto; decisiónhost/dibujante doblados. GuardasC y cálculo del dibujo original se ejecutan aparte; sinGUI.')
