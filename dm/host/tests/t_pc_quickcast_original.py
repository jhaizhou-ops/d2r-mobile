"""4ace20→4ae3b0/4ad500→4af320/4ad870 originales.
Mismo actor/selección395/XY, cambia únicamente bit0 del quinto argumento.
Actor, target, preferencias, disponibilidad/rango y navmesh son providers
explícitos. 4b83b0 es sink de acción, no se ejecutan servidor/maná/GPU.
"""
from pathlib import Path
import struct,hashlib,json,argparse
parser=argparse.ArgumentParser();parser.add_argument("--host-log",type=Path);parser.add_argument("--json",type=Path);args=parser.parse_args()
host={};predicate={};standstill={}
if args.host_log:
 for line in args.host_log.read_text().splitlines():
  if line.startswith("NATIVE_QUICKCAST "):
   _,quick,side,phase,flags=line.split();host[int(quick),int(side),int(phase)]=int(flags)
  elif line.startswith("NATIVE_STANDSTILL "):
   _,stand,side,phase,flags=line.split();standstill[int(stand),int(side),int(phase)]=int(flags)
  elif line.startswith("NATIVE_PREDICATE "):
   _,skill,flags=line.split();predicate[int(skill)]=int(flags)
 assert len(host)==12,"Se requieren las doce fases/intenciones producidas por C"
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r
b=Path('dm/host/tests/build/main.orig.bin').read_bytes();assert hashlib.sha256(b).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
results=[]
for side in (0,1):
 for flags in (0,1):
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,0x4000000);u.mem_write(0,b);u.mem_map(0x5000000,0x100000)
  player,manager,path,skills,node,record,option,functor,table,cap=[0x5000000+i*0x1000 for i in range(10)]
  record=0x5080000+395*0x2ec
  sink=0x50c0000;stop=0x50ff000
  def w(a,v,f='Q'):u.mem_write(a,struct.pack('<'+f,v))
  def q(a,f='Q'):return struct.unpack('<'+f,u.mem_read(a,struct.calcsize(f)))[0]
  def x(i):return u.reg_read(getattr(r,'UC_ARM64_REG_X'+str(i)))
  def ret(v=0):u.reg_write(r.UC_ARM64_REG_X0,v);u.reg_write(r.UC_ARM64_REG_PC,u.reg_read(r.UC_ARM64_REG_LR))
  w(player,0,'I');w(player+8,1,'I');w(player+0xc,1,'I');w(player+0x38,path);w(player+0x100,skills);w(player+0x1bd,1,'B')
  w(path+0x10,5736,'I');w(path+0x14,5953,'I');w(path+2,5736,'H');w(path+6,5953,'H')
  w(skills,node);w(skills+8,node);w(skills+16,node);w(node,record);w(node+0x38,1,'I');w(node+0x44,-1,'i');w(record,395,'h');w(record+0x27,0x40,'B');w(record+0x28,0x80,'B')
  w(0x2d60380,1,'I');w(manager+0xd44,-1,'i');w(manager+0xd4c,-1,'i');w(option+0xc,0,'I');w(option+0xc8,functor);w(option+0x118,functor);w(functor+0x10,sink)
  w(0x2d64a28,table);w(table+0x11e8,0x5080000);w(table+0x11f0,500,'I');w(table+0x1500,cap);w(cap,100,'I');events=[];phase=''
  def hook(cpu,at,size,data):
   if at in (0x2acecc,):ret(manager)
   elif at==0x45f9d0:ret(player)
   elif at==0x310000:ret(option)
   elif at in (sink,0x2f3880,0x59fa20,0x5b25c0):ret(0)
   elif at in (0x466350,0x4af140,0x4ae230):
    if at==0x4ae230:w(x(2),0,'I')if x(2)else None
    ret(1)
   elif at in (0x2f3a40,0x2f3b90):ret(0)
   elif at in (0x4ad870,0x4af320):events.append(dict(phase=phase,boundary=hex(at),flags=q(x(0),'I')if at==0x4af320 else None))
   elif at==0x4b83b0:events.append(dict(phase=phase,boundary=hex(at),action=x(0),player=hex(x(1)),xy=[x(2)&0xffffffff,x(3)&0xffffffff]));ret(1)
  u.hook_add(UC_HOOK_CODE,hook)
  def run(action,native_flags):
   u.reg_write(r.UC_ARM64_REG_SP,0x50ef000);u.reg_write(r.UC_ARM64_REG_LR,stop)
   for i,v in enumerate((player,action,5736,5943,native_flags)):u.reg_write(getattr(r,'UC_ARM64_REG_X'+str(i)),v)
   try:u.emu_start(0x4ace20,stop,count=20000)
   except Exception:print('ERROR',side,flags,phase,hex(u.reg_read(r.UC_ARM64_REG_PC)));raise
   assert u.reg_read(r.UC_ARM64_REG_PC)==stop
  for step in (0,1):
   phase='DOWN'if step==0 else'HELD';native_flags=host[flags,side,step]if host else flags;run(side*3+step,native_flags)
  move=sum(e['boundary']=='0x4ad870'for e in events);cast=sum(e['boundary']=='0x4af320'for e in events)
  assert (move,cast)==((2,0)if side==0 and not flags else(0,2)),(side,flags,events)
  phase='UP';before=len(events);w(player+0xc,6,'I');run(side*3+2,host[flags,side,2]if host else 0)
  assert q(0x2d635f0,'I')==0
  released=events[before:]
  assert not any(e['boundary']=='0x4af320'for e in released)
  assert bool(released)==(side==0 and not flags),(side,flags,released)
  results.append(dict(side=side,quickcast=flags,events=events));print('PASS lado',side,'quickcast',flags,'move',move,'cast',cast,'UPcontinuación',bool(released),'acciones',[e.get('action')for e in events if'action'in e])
print('PASS4/4: LMB flags0 camina; sólo bit0 local cambia DOWN/HELD a4af320. RMB flags0 ya usa cast. Selección/nodo constantes. Sin afirmar consumo de maná o impacto real.')
if args.json:args.json.write_text(json.dumps(results,indent=2)+'\n')

# El productor original de quickcast prepara este bit incluso sin StandStill físico.
for skill,expected in ((0,1),(2,1),(3,1),(4,1),(356,1),(357,0),(358,0),(359,1),(364,1),(369,1),(370,0),(371,1),(395,1)):
 w(manager+0xd4c,skill,'I');w(0x2d129d0+0xd44,0xffffffff,'I');w(0x2d129d0+0xd4c,0xffffffff,'I')
 out=0x50e0000;w(out,0xab,'B')
 u.reg_write(r.UC_ARM64_REG_SP,0x50ef000);u.reg_write(r.UC_ARM64_REG_LR,stop)
 for i,v in enumerate((manager,0,out)):u.reg_write(getattr(r,'UC_ARM64_REG_X'+str(i)),v)
 u.emu_start(0x2b2254,stop,count=5000)
 assert q(out,'B')==expected,(skill,q(out,'B'))
 if predicate:assert predicate[skill]==q(out,'B'),(skill,predicate[skill],q(out,'B'))
 print('PASS productor QuickCast original2b2254 skill',skill,'bit',expected)
# Acción36 reasignable: RELA identifica los callbacks de la tabla69 sin usar VKShift.
assert struct.unpack_from('<QQQ',b,0x1bbdfe0)==(0x20866e8,0x403,0x369c30)
assert struct.unpack_from('<QQQ',b,0x1bbdff8)==(0x20866f0,0x403,0x369c40)
assert (0x20866e8-0x2086388)//24==36
for callback,expected in ((0x369c30,1),(0x369c40,0)):
 u.reg_write(r.UC_ARM64_REG_LR,stop);u.emu_start(callback,stop,count=20)
 assert q(0x2d20e5c,'I')==expected
 for side,start,end in ((0,0x2c1ab4,0x2c1b28),(1,0x2c1b5c,0x2c1bd0)):
  u.reg_write(r.UC_ARM64_REG_X0,player);u.reg_write(r.UC_ARM64_REG_X19,0x2d20000)
  u.reg_write(r.UC_ARM64_REG_SP,0x50ef000);u.reg_write(r.UC_ARM64_REG_LR,stop)
  def convert(cpu,at,size,data):
   if at==0x2ba3f0:ret(1)
  h=u.hook_add(UC_HOOK_CODE,convert);u.emu_start(start,end,count=500);u.hook_del(h)
  assert u.reg_read(r.UC_ARM64_REG_PC)==end and x(4)&1==expected
  if standstill:assert standstill[expected,side,1]==x(4)&1
  w(manager+0xd48,int(not side),'B');u.reg_write(r.UC_ARM64_REG_X20,manager);u.reg_write(r.UC_ARM64_REG_X21,player);u.reg_write(r.UC_ARM64_REG_X22,0)
  u.emu_start(0x2b5a90,0x2b5ae4,count=500)
  assert x(4)&1==expected and x(1)==side*3+2
  if standstill:assert standstill[expected,side,2]==x(4)&1
  print('PASS StandStill acción36 callback',hex(callback),'mano',side,'HELD/UP nativos bit',expected)
print('PASS subtotal21: cuatro trayectorias DOWN/HELD/UP, trece productores quickcast, cuatro callbacks+productor mouse ambas manos. Endpoint4b83b0/providers explícitos; no servidor/maná/GPU.')

# Medir la rama target del helper sin transferir sus side effects al port.
# El contrato puro termina en2b22ac; metadata1a+validator pueden sobrescribirlo después.
for target_present,metadata,accepted in ((0,0,0),(1,0,0),(1,1,0),(1,1,1)):
 w(manager+0xd4c,395,'I');w(manager+0x1610,0x50a0000);w(0x50a0000+395*0x78+0x1a,metadata,'B');w(manager+0xd74,0,'I')
 def target_provider(cpu,at,size,data):
  if at==0x2f3880:ret(0x500a000 if target_present else 0)
  elif at==0x2fb8cc:
   assert x(0)==manager and x(1)==player and x(2)==0x500a000
   ret(accepted)
 h=u.hook_add(UC_HOOK_CODE,target_provider)
 u.reg_write(r.UC_ARM64_REG_SP,0x50ef000);u.reg_write(r.UC_ARM64_REG_LR,stop)
 for i,v in enumerate((manager,0,out)):u.reg_write(getattr(r,'UC_ARM64_REG_X'+str(i)),v)
 u.emu_start(0x2b2254,stop,count=5000);u.hook_del(h)
 overridden=target_present and metadata and accepted
 assert q(out,'B')==int(not overridden) and q(manager+0xd74,'I')==(2 if overridden else 0)
 print('TARGET original',target_present,metadata,accepted,'flag_final',q(out,'B'),'manager_d74',q(manager+0xd74,'I'),'pure_C',predicate.get(395,1),'override_posterior',int(overridden))
print('PASS total25. Rama target final es distinta del predicado puro autorizado cuando metadata1a y validator aceptan; queda explícita, sin copiar manager+d74.')

source=Path('dm/host/dm_world.c').read_text()
assert 'return pc_world_mouse(secondary,phase,nx,ny,-1);'in source
assert 'pc_world_mouse(side,phase,x,y,g_pc_skill.active?g_pc_skill.skill:-1)'in source
assert 'return pc_mouse_emit(&c,&api,secondary,phase,tile,quickcast_skill);'in source
assert 'c.standstill=dm_ui_pc_binding_held(36);'in source
assert 'if(!enabled || skill<0 || (skill>=357&&skill<=364) || skill==370){select_slot(slot,0);return;}'in source
print('PASS5guardas estructurales: enrutado físico/skill/emisor/estado nativo/QuickCastOFF. No sustituyen ejecución del caller en GUI.')
