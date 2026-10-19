"""Contratos PC del binario original: paneles independientes y herencia; sin app ni saves."""
from pathlib import Path
import hashlib
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

image=bytearray(Path(sys.argv[1]).read_bytes())
assert hashlib.sha256(image).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
assert struct.unpack_from('<4I',image,0x459ad0)==(0xd10183ff,0xa9025ff8,0xa90357f6,0xa9044ff4)
for pos in range(0x1bf0000,0x1c30000,8):
 address,kind,value=struct.unpack_from('<QQQ',image,pos)
 if kind==0x403 and 0x20c3f78<=address<0x20c4128:struct.pack_into('<Q',image,address,value)
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,0x4000000);u.mem_write(0,bytes(image));u.mem_map(0x5000000,0x1000000)
root,vt,allocvt,alloc,release,dispatch,end=0x5000000,0x5001000,0x5002000,0x5003000,0x5004000,0x5005000,0x5006000
heap=0x5100000;events=[];paths=[];resource_present=1
def q(a,v):u.mem_write(a,struct.pack('<Q',v))
def z(a):return struct.unpack('<Q',u.mem_read(a,8))[0]
def text(a):
 out=bytearray()
 while (v:=u.mem_read(a+len(out),1))!=b'\0':out.extend(v)
 return bytes(out)
q(0x2f05458,root);q(root,vt);q(vt+0x28,dispatch);u.mem_write(root+0xb9,b'\1')
q(0x2f05398,allocvt);q(allocvt+0x10,alloc);q(allocvt+0x28,release)
u.mem_write(0x3211700,b'\1');q(0x32116f8,0x500a000);q(0x500a000,allocvt)
q(0x2d20b80,0x5007000);u.mem_write(0x500705c,b'\1')
def hook(cpu,address,size,opaque):
 global heap
 a,b,c=[cpu.reg_read(getattr(r,f'UC_ARM64_REG_X{i}')) for i in range(3)]
 result=None
 if address==0xaaec08:result=a
 elif address==0x459500:result=1
 elif address==0x45f9d0:result=0
 elif address in (0xabd6bc,0x36b370,0x44b610,0x3703c0,0x433d90,release):result=0
 elif address==0x1b76070:result=len(text(a))
 elif address in (0x1b76040,0x1b760c0):u.mem_write(a,bytes(u.mem_read(b,c)));result=a
 elif address==0x1b6f300:
  value=text(c)[:b-1]+b'\0';u.mem_write(a,value);result=len(value)-1
 elif address==0x1af8040:paths.append(text(z(a)));result=resource_present
 elif address==alloc:result=heap;heap+=(b+15)&~15
 elif address==dispatch:events.append((z(b),z(b+8),text(z(b+16))));result=0
 if result is not None:
  cpu.reg_write(r.UC_ARM64_REG_X0,result);cpu.reg_write(r.UC_ARM64_REG_PC,cpu.reg_read(r.UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def run(address,x0=0,x1=0):
 u.reg_write(r.UC_ARM64_REG_SP,0x5fff000);u.reg_write(r.UC_ARM64_REG_LR,end);u.reg_write(r.UC_ARM64_REG_X0,x0);u.reg_write(r.UC_ARM64_REG_X1,x1);u.emu_start(address,end,count=100000)
 assert u.reg_read(r.UC_ARM64_REG_PC)==end
 return u.reg_read(r.UC_ARM64_REG_X0)
for hd in (0,1):
 u.mem_write(root+0xb9,bytes([hd]));paths.clear();name=b'HUDPanel\0';u.mem_write(0x5009000,name);q(0x5008000,0x5009000);q(0x5008008,len(name)-1);q(0x5008010,0xffffffffffffffff)
 before=bytes(u.mem_read(0x5008000,24));resource_present=1
 assert run(0xb88010,0x5008000)==1 and paths==[b'data/global/ui/Layouts/HUDPanel'+(b'HD.json' if hd else b'.json')]
 assert bytes(u.mem_read(0x5008000,24))==before
 resource_present=0;assert run(0xb88010,0x5008000)==0
 print(f'PASS existencia original HD={hd}: objeto prestado intacto; sufijo/perfil original y ausencia reconocidos')
u.mem_write(root+0xb9,b'\1')
widget=run(0xbeb314)
assert z(widget)==0x20c4008 and z(0x20c4008+0x28)==0xaaf9b4 and z(0x20c4008+0xc0)==0xbeaf94
print('PASS fábrica MiniMenuButtonWidget original, vtable e InputMessage/callback registrados')
for panel in (14,2,1,4,9):
 events.clear();assert run(0x459ad0,panel)==1
 assert u.mem_read(0x2d5ff48+panel,1)==b'\1'
 assert len(events)==1 and events[0][0]==0x7127a406e0b6452c
 opened=events[0];events.clear();assert run(0x459ad0,panel)==1
 assert u.mem_read(0x2d5ff48+panel,1)==b'\0'
 assert len(events)==1 and events[0][2]==opened[2] and events[0][1]!=opened[1]
 assert opened[2]!=b'UISwitcher'
 print(f'PASS toggle original {panel}: abrir/cerrar {opened[2].decode()}, sin UISwitcher')
# basedOn se abre dentro del loader; no vuelve al selector de prefijos.
cs=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
assert not any(i.mnemonic=='bl' and i.op_str=='#0xb1cd34' for i in cs.disasm(bytes(image[0xb84750:0xb87000]),0xb84750))
print('PASS basedOn: cargador original sin llamada al selector de prefijos')
print('LÍMITE: validación/elegibilidad, allocator y envío final son fronteras simuladas; no prueba GPU ni clic completo.')
