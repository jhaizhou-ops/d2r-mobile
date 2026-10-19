"""Puertas9f/a0 del producto: ramas/GPR/SP/NZCV; fronteras host simuladas."""
from pathlib import Path
import hashlib
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r
p=Path(sys.argv[1]);image=(p/'main.patched.bin').read_bytes()
rows=[line.split('\t') for line in (p/'main.gates.tsv').read_text().splitlines() if line and not line.startswith('#')]
headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0) for line in (p/'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
regs=[getattr(r,f'UC_ARM64_REG_X{i}') for i in range(31)]
print('SHA256',hashlib.sha256(image).hexdigest())
for number,site in ((0x9f,0x3b8648),(0xa0,0x3db0b4)):
 ret=next(int(row[0],16) for row in rows if row[1:]==['svc',str(number),hex(site)])
 for handled in (0,1):
  for inside in (0,1):
   u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,image)
   for page in (0x7000000,0x8000000,0x9000000):u.mem_map(page,0x10000)
   u.mem_write(headers['KEY_PAGE_OFF']+headers['KEY_KGATE_IDX']*8,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
   sp=0x7008000;flags=0xa0000000|(inside<<30);u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags)
   values=[0x1110000+i*16 for i in range(31)];values[0]=inside
   for reg,value in zip(regs,values):u.reg_write(reg,value)
   calls=[]
   def host(cpu,address,size,opaque):
    if address==0x9000000:
     calls.append(address);assert cpu.reg_read(r.UC_ARM64_REG_LR)==ret
     assert [cpu.reg_read(regs[i]) for i in (0,8,19,20,23)]==[values[i] for i in (0,8,19,20,23)]
     cpu.reg_write(regs[8],handled);cpu.reg_write(r.UC_ARM64_REG_PC,ret)
   u.hook_add(UC_HOOK_CODE,host)
   target=(0x3b8664 if handled else site+4) if number==0x9f else (0x3db198 if handled else 0x3db2b4 if inside else site+4)
   u.emu_start(site,target,count=100)
   expected=values[:]
   if number==0x9f and not handled:expected[8]=0x2d12000
   assert calls==[0x9000000] and [u.reg_read(reg) for reg in regs]==expected
   assert u.reg_read(r.UC_ARM64_REG_SP)==sp and u.reg_read(r.UC_ARM64_REG_NZCV)==flags
   print(f'PASS {number:x} handled={handled} inputbit={inside}: destino={target:x}, registros/SP/NZCV y relocalización original')
print('PASS8 producto9f/a0; host simulado. Cola/explicit-drop y NPC panel→fila→servicio originales se ejecutan en arneses separados. Sin GUI.')
