"""Puertas9d/9e reales: LeftDown/ShiftRightDown, fallback y consumo original. Host simulado."""
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
for number,site in ((0x9d,0x3ca47c),(0x9e,0x3ca58c)):
 ret=next(int(row[0],16) for row in rows if row[1:]==['svc',str(number),hex(site)])
 for handled in (0,1):
  for equal in (0,1):
   u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,image)
   for page in (0x7000000,0x8000000,0x9000000):u.mem_map(page,0x10000)
   u.mem_write(headers['KEY_PAGE_OFF']+headers['KEY_KGATE_IDX']*8,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
   sp=0x7008000;flags=0xa0000000|(equal<<30);u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags)
   values=[0x1110000+i*16 for i in range(31)];values[27]=0x2f05000
   if number==0x9e:values[0]=values[20];values[8]=0x9000200
   for reg,value in zip(regs,values):u.reg_write(reg,value)
   calls=[];native=[]
   def host(cpu,address,size,opaque):
    if address==0x9000000:
     calls.append(address);assert cpu.reg_read(r.UC_ARM64_REG_LR)==ret
     assert cpu.reg_read(regs[20])==values[20] and cpu.reg_read(regs[22])==values[22]
     cpu.reg_write(regs[8],handled);cpu.reg_write(r.UC_ARM64_REG_PC,ret)
    elif address==0x9000200:
     native.append(address);assert cpu.reg_read(r.UC_ARM64_REG_LR)==site+4
     assert cpu.reg_read(regs[0])==values[20] and cpu.reg_read(regs[1])==values[1]
     cpu.reg_write(r.UC_ARM64_REG_PC,site+4)
   u.hook_add(UC_HOOK_CODE,host)
   target=(0x3ca590 if handled else 0x3ca5a0 if equal else site+4) if number==0x9d else site+4
   u.emu_start(site,target,count=100)
   expected=values[:]
   if number==0x9e and not handled:expected[30]=site+4
   assert calls==[0x9000000] and [u.reg_read(reg) for reg in regs]==expected
   assert native==([0x9000200] if number==0x9e and not handled else [])
   assert u.reg_read(r.UC_ARM64_REG_SP)==sp and u.reg_read(r.UC_ARM64_REG_NZCV)==flags
   if handled:
    root=0x9001000;u.mem_write(0x2f05458,struct.pack('<Q',root));u.mem_write(root+0xb8,b'\0')
    u.emu_start(0x3ca590,0x3ca5a0,count=10)
    assert bytes(u.mem_read(root+0xb8,1))==b'\1'
   print(f'PASS {number:x} handled={handled} Z={equal}: registros/SP/NZCV, fallback y consumo invitado')
print('PASS 8 casos producto9d/9e. Frontera host simulada; F8/dominio original y pruebas C se ensayan aparte; sin GUI.')
