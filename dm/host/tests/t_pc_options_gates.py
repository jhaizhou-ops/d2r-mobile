"""Puertas97/98 del producto oficial; host/modelo simulados explícitamente."""
from pathlib import Path
import hashlib
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r

product=Path(sys.argv[1]);image=(product/'main.patched.bin').read_bytes()
rows=[line.split('\t') for line in (product/'main.gates.tsv').read_text().splitlines() if line and not line.startswith('#')]
headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0) for line in (product/'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
registers=[getattr(r,f'UC_ARM64_REG_X{i}') for i in range(31)]
print('SHA256',hashlib.sha256(image).hexdigest())
passed=0
for desktop in (0,1):
 for site,number in ((0xae950c,0x97),(0xbb59b4,0x98)):
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,image)
  for page in (0x7000000,0x8000000,0x9000000):u.mem_map(page,0x10000)
  u.mem_write(headers['KEY_PAGE_OFF']+headers['KEY_KGATE_IDX']*8,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000))
  u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100);u.reg_write(r.UC_ARM64_REG_SP,0x7008000);u.reg_write(r.UC_ARM64_REG_NZCV,0xa0000000)
  values=[0x1110000+i*16 for i in range(31)];values[19]=0x8004000
  if number==0x97:values[23]=0x2f05000;u.mem_write(0x2f05458,struct.pack('<Q',0x8005000))
  else:values[0]=values[19]
  for reg,value in zip(registers,values):u.reg_write(reg,value)
  matching=[int(row[0],16) for row in rows if row[1:]==['svc',str(number),hex(site)]];assert len(matching)==1,matching
  ret=matching[0];calls=[];replayed=[]
  def hook(cpu,address,size,opaque):
   if address==0x9000000:
    assert cpu.reg_read(r.UC_ARM64_REG_LR)==ret
    assert cpu.reg_read(registers[19])==values[19]
    if number==0x97:cpu.reg_write(registers[8],0)
    else:
     assert cpu.reg_read(registers[0])==values[19]
     cpu.reg_write(registers[0],0xbeef)
    calls.append((number,desktop));cpu.reg_write(r.UC_ARM64_REG_PC,ret)
   elif address>=headers['PATCH_OFF'] and struct.unpack('<I',cpu.mem_read(address,4))[0]==0xf9422ee8:replayed.append(address)
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(site,site+4,count=100)
  expected=values[:];expected[8 if number==0x97 else 0]=0x8005000 if number==0x97 else 0xbeef
  assert calls==[(number,desktop)] and [u.reg_read(reg) for reg in registers]==expected
  assert u.reg_read(r.UC_ARM64_REG_SP)==0x7008000 and u.reg_read(r.UC_ARM64_REG_NZCV)==0xa0000000
  assert len(replayed)==(number==0x97)
  print('PASS',hex(number),'PC',desktop,'self/return/GPR/SP/NZCV; replay',len(replayed));passed+=1
print('PASS',passed,'product gate ABI cases; host policy/original init are explicit sinks; no GUI or preference writes')
