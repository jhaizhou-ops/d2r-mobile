"""Puerta PC de selección hover: ABI real del producto; frontera host simulada."""
from pathlib import Path
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r
product=Path(sys.argv[1]);data=(product/'main.patched.bin').read_bytes()
def rows(p):return [s.split('\t')for s in p.read_text().splitlines()if s and not s.startswith('#')]
ret=next(int(row[0],16)for row in rows(product/'main.gates.tsv')if row[1:]==['svc','146','0xb0d150'])
headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0)for line in(product/'main.fixups.tsv').read_text().splitlines()if line.startswith('# ')}
for desktop in (0,1):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(data)+4095)&~4095);u.mem_write(0,data)
 for page in (0x7000000,0x8000000,0x9000000):u.mem_map(page,0x10000)
 u.mem_write(headers['KEY_PAGE_OFF']+headers['KEY_KGATE_IDX']*8,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000))
 u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100);u.reg_write(r.UC_ARM64_REG_SP,0x7008000);u.reg_write(r.UC_ARM64_REG_NZCV,0xa0000000)
 values={i:0x1110000+i*16 for i in range(31)};values[8]=0x2d12000
 for i,v in values.items():u.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),v)
 calls=[]
 def host(cpu,pc,size,opaque):
  if pc==0x9000000:
   calls.append(pc);assert cpu.reg_read(r.UC_ARM64_REG_LR)==ret;cpu.reg_write(r.UC_ARM64_REG_X8,desktop);cpu.reg_write(r.UC_ARM64_REG_PC,ret)
 u.hook_add(UC_HOOK_CODE,host);target=0xb0d168 if desktop else 0xb0d154;u.emu_start(0xb0d150,target,count=100)
 assert len(calls)==1 and u.reg_read(r.UC_ARM64_REG_SP)==0x7008000 and u.reg_read(r.UC_ARM64_REG_NZCV)==0xa0000000
 assert u.reg_read(r.UC_ARM64_REG_PC)==target
 for i,v in values.items():assert u.reg_read(getattr(r,f'UC_ARM64_REG_X{i}'))==(v+0x770 if i==8 and not desktop else v)
 print(f'PASS hover gate PC={desktop}: destino{target:x}, ADD original solo legacy, registros/SP/NZCV conservados')
print('LÍMITE: función host simulada; productor A/B/C y ClearInfo/listUpdate se validan en arnés original independiente.')
