"""Ejecuta en Unicorn la API original de foco de 3.3.94012, sin abrir la app."""
from pathlib import Path
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as reg

image = Path(sys.argv[1]).read_bytes()
start = 0xABE064
assert struct.unpack_from('<4I', image, start) == (0xF81C0FF7,0xA90157F6,0xA9024FF4,0xA9037BFD)
uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
for addr in [0xABE000,0x400000,0x500000,0x510000,0x520000,0x530000,0x540000,0x550000,0x2D12000]:
    uc.mem_map(addr,0x1000)
uc.mem_write(0xABE000,image[0xABE000:0xABF000])
def w64(addr,value): uc.mem_write(addr,struct.pack('<Q',value))
def r64(addr): return struct.unpack('<Q',uc.mem_read(addr,8))[0]
manager,container,selected,vtable = 0x500000,0x510000,0x520000,0x530000
uc.mem_write(manager+0x180,b'\x01')
w64(manager+0x188,container)
w64(manager+0x190,selected)
w64(manager+0x198,selected)
w64(container+0x160,selected)
w64(selected,vtable)
w64(vtable+0x78,0x540000)
w64(vtable+0x70,0x540004)
uc.mem_write(0x540000,struct.pack('<2I',0xD65F03C0,0xD65F03C0))
uc.mem_write(0x2D128F0,b'\x01')
calls = {0x540000:0,0x540004:0}
def hook(engine,address,size,user):
    if address in calls: calls[address]+=1
uc.hook_add(UC_HOOK_CODE,hook)
def select(target):
    uc.reg_write(reg.UC_ARM64_REG_SP,0x400F00)
    uc.reg_write(reg.UC_ARM64_REG_X0,manager+0x180)
    uc.reg_write(reg.UC_ARM64_REG_X1,container)
    uc.reg_write(reg.UC_ARM64_REG_X2,target)
    uc.reg_write(reg.UC_ARM64_REG_X30,0x550000)
    uc.emu_start(start,0x550000,count=1000)
select(0)
assert calls[0x540000]==1 and r64(manager+0x190)==0
assert r64(manager+0x188)==container
assert r64(manager+0x198)==selected and r64(container+0x160)==selected
select(selected)
assert calls[0x540004]==1 and r64(manager+0x190)==selected
assert r64(manager+0x198)==selected and r64(container+0x160)==selected
print('AUDIO FOCUS NATIVE PASS: FocusLost, memoria conservada y FocusGained original')
