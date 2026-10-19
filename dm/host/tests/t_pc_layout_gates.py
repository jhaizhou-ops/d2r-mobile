"""Comprueba puertas PC del producto aislado y ABI original; no inicia el juego."""
from pathlib import Path
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r

product,previous=map(Path,sys.argv[1:])
def rows(p):return [s.split('\t') for s in p.read_text().splitlines() if s and not s.startswith('#')]
def branch(b,a):
 w=struct.unpack_from('<I',b,a)[0];assert w&0xfc000000==0x14000000
 d=w&0x3ffffff;return a+4*(d-(1<<26) if d&(1<<25) else d)
image=(product/'main.patched.bin').read_bytes();before=rows(previous/'main.obs.tsv');after=rows(product/'main.obs.tsv')
assert after[:-2]==before
assert [row[:3] for row in after[-2:]]==[['0xb88010','24','f81a0ffc'],['0xb84750','25','a9ba6ffc']]
for name in ('sdk','subsdk0'):assert (product/f'{name}.patched.bin').read_bytes()==(previous/f'{name}.patched.bin').read_bytes()
headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0) for line in (product/'main.fixups.tsv').read_text().splitlines() if line.startswith('# ') }
for address,identifier,first in [(0xb88010,24,'fc0f1af8'),(0xb84750,25,'fc6fbaa9')]:
 start=branch(image,address);ret=next(int(row[0],16) for row in rows(product/'main.gates.tsv') if row[1:]==['obs',str(identifier),hex(address)])
 assert image[ret+4:ret+8]==bytes.fromhex(first) and branch(image,ret+8)==address+4
 for replace in (False,True):
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,image)
  for page in (0x7000000,0x8000000,0x9000000):u.mem_map(page,0x10000)
  u.mem_write(headers['KEY_PAGE_OFF']+headers['KEY_KGATE_IDX']*8,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000))
  u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100);u.reg_write(r.UC_ARM64_REG_SP,0x7008000);u.reg_write(r.UC_ARM64_REG_NZCV,0xa0000000)
  values={i:0x1110000+i*16 for i in range(31)}
  for i,v in values.items():u.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),v)
  calls=[]
  def gate(cpu,pc,size,opaque):
   if pc!=0x9000000:return
   calls.append(pc)
   assert cpu.reg_read(r.UC_ARM64_REG_LR)==ret
   if replace:cpu.reg_write(r.UC_ARM64_REG_X0,0xbeef100)
   cpu.reg_write(r.UC_ARM64_REG_PC,ret)
  u.hook_add(UC_HOOK_CODE,gate);u.emu_start(address,address+4,count=100)
  assert len(calls)==1 and u.reg_read(r.UC_ARM64_REG_SP)==0x7008000-0x60
  assert u.reg_read(r.UC_ARM64_REG_NZCV)==0xa0000000
  for i,v in values.items():assert u.reg_read(getattr(r,f'UC_ARM64_REG_X{i}'))==(0xbeef100 if replace and i==0 else v)
  assert struct.unpack('<Q',u.mem_read(0x7008000-0x60,8))[0]==values[28]
  if identifier==25:assert struct.unpack('<Q',u.mem_read(0x7008000-0x58,8))[0]==values[27]
 print(f'PASS OBS{identifier}: x0 adaptable; perfilw1/padrex2 y todos los demás registros/flags intactos; prólogo una vez')
print('PASS producto aislado: solo dos OBS nuevas; sdk/subsdk0 anteriores idénticos')
