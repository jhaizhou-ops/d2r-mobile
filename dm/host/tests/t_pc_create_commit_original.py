"""Rutas originales de commit: cuadro PC frente a TryCreateCharacter Switch.

Ejecuta handler, string assign, validadores de nombre y constructor de solicitud.
PLT/allocator, capacidad del servicio y entrega asíncrona son periféricos doblados.
No escribe archivos ni configura un héroe desde el host.
"""
from pathlib import Path
import hashlib,struct,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r
B=Path('dm/host/tests/build/main.orig.bin').read_bytes()
assert hashlib.sha256(B).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
def fnv(s):
 h=0xcbf29ce484222325
 for c in s.encode():h=((h^c)*0x100000001b3)&0xffffffffffffffff
 return h
def run(route,text='PruebaPC',enum=3,version=2,bridge=False,service='none'):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,0x4000000);u.mem_write(0,B);u.mem_map(0x5000000,0x100000)
 panel,msg,box,name,entered,node,classname,alloc,vt=range(0x5000000,0x5009000,0x1000)
 def q(p,*v):u.mem_write(p,struct.pack('<'+'Q'*len(v),*v))
 def string(p):return bytes(u.mem_read(p,1024)).split(b'\0',1)[0]
 u.mem_write(panel+0x50,b'\1\1');u.mem_write(panel+0x19c,bytes([version]));q(panel+0x1a0,name,0,0x800000000000003f);u.mem_write(name,b'\0')
 u.mem_write(entered,text.encode()+b'\0');q(box+0x558,entered,len(text.encode()));q(panel+0x160,box);q(panel+0x190,2)
 u.mem_write(classname,b'Paladin\0');q(0x2f14240,classname,7);q(0x2f142c0,node);q(node+0x20,classname,7);u.mem_write(node+0x38,bytes([enum]));u.mem_write(0x2d7badc,struct.pack('<I',1))
 q(alloc,vt);q(vt+0x10,0x500a000);q(vt+0x28,0x500a004);q(0x32116f8,alloc);u.mem_write(0x3211700,b'\1')
 if route in ('PC','Enter','Confirmed'):q(msg,fnv('CharacterCreate'),fnv('ConfirmedCreate' if route=='Confirmed' else 'Create'),0,1)
 else:q(msg,fnv('ConsoleCharacterCreate'),fnv('TryCreateCharacter'),entered,1)
 if route=='Confirmed':u.mem_write(name,text.encode()+b'\0');q(panel+0x1a8,len(text.encode()))
 root,rootvt,button,payload,bridge_msg=0x5009000,0x500b000,0x500c000,0x500d000,0x500e000
 if route=='Enter':
  q(root,rootvt);q(panel,rootvt);q(rootvt+0x28,0xc267c8);q(0x2f05458,root);u.mem_write(root+0xb9,b'\1');q(panel+0x168,button);q(msg,fnv('InputMessage'),fnv('KeyDown'),payload,1);u.mem_write(payload+0x10,struct.pack('<I',13))
 # El constructor usa estos VT de functors al construir la petición asíncrona.
 for p in (0x211ff50,0x2122fb0,0x2122f28):q(p,vt)
 q(vt+0x10,0x500a000);q(vt+0x28,0x500a004);q(vt+0x0,0x500a004)
 u.mem_write(0x2c875f0,b'\1');u.mem_write(0x315e9a8,b'\0'*4);heap=0x5050000;calls=[];result={};history=[]
 def hook(c,at,size,data):
  nonlocal heap
  history.append(at)
  if len(history)>100:history.pop(0)
  x=[c.reg_read(z) for z in (r.UC_ARM64_REG_X0,r.UC_ARM64_REG_X1,r.UC_ARM64_REG_X2)]
  value=None
  if at==0xc27764:calls.append('native_enter_to_create')
  elif at==0xc26e0c and bridge:
   active=c.reg_read(r.UC_ARM64_REG_X20);cat,event=struct.unpack('<QQ',c.mem_read(active,16))
   if (cat,event)==(fnv('CharacterCreate'),fnv('Create')):
    q(bridge_msg,fnv('ConsoleCharacterCreate'),fnv('TryCreateCharacter'),entered,1);calls.append('host_A4_adapter');c.reg_write(r.UC_ARM64_REG_X0,panel);c.reg_write(r.UC_ARM64_REG_X1,bridge_msg);c.reg_write(r.UC_ARM64_REG_LR,at+4);c.reg_write(r.UC_ARM64_REG_PC,0xc267c8);return
  elif at in (0x1b76040,0x1b760c0):c.mem_write(x[0],bytes(c.mem_read(x[1],x[2])));value=x[0]
  elif at==0x1b76070:value=len(string(x[0]))
  elif at==0x1b770d0:value=len(string(x[0])[:x[1]])
  elif at==0x1b772f0:value=int(chr(x[0]&255).isascii() and chr(x[0]&255).isalpha())
  elif at==0x1b76240:
   a,z=bytes(c.mem_read(x[0],x[2])),bytes(c.mem_read(x[1],x[2]));value=(a>z)-(a<z)
  elif at==0x500a000:value=heap;heap+=0x1000
  elif at==0x500a004:value=0
  elif at==0xe16ca0:
   request=0x500f000 if service!='none' else 0;q(c.reg_read(r.UC_ARM64_REG_X8),request);calls.append('native_name_service_'+service);value=0
   if request:q(request,vt);c.mem_write(request+0x28,struct.pack('<H',2));q(request+0x40,entered,len(text.encode()));c.mem_write(request+0x458,struct.pack('<H',1))
  elif at==0x171830:value=1 # Capacidad del proveedor offline, no validación de nombre.
  elif at==0xc23624:value=1 # Disponibilidad clase/versión, fuera del commit de nombre.
  elif at==0xc25bb4:result['name']=string(0x212730d).decode();result['class']=c.mem_read(0x2127c47,1)[0];result['version']=c.mem_read(0x2127c48,1)[0];calls.append('offline_native_constructor')
  elif at==0x179ec8:
   result['request_name']=string(x[1]+0x60).decode();calls.append('native_async_provider');c.emu_stop();return
  elif at in (0x7f5630,0xb168e0,0xb15d60):value=entered
  elif at in (0x868810,0xb7a6dc):value=0
  elif at==0xb17110:result['validation_popup']=True;c.emu_stop();return
  elif at in (0x1b76d20,0x1b76d40):value=0
  else:return
  if value is not None:c.reg_write(r.UC_ARM64_REG_X0,value&0xffffffffffffffff);c.reg_write(r.UC_ARM64_REG_PC,c.reg_read(r.UC_ARM64_REG_LR))
 u.hook_add(UC_HOOK_CODE,hook);u.reg_write(r.UC_ARM64_REG_X0,panel);u.reg_write(r.UC_ARM64_REG_X1,msg);u.reg_write(r.UC_ARM64_REG_SP,0x50ff000);u.reg_write(r.UC_ARM64_REG_LR,0x50f0000)
 try:u.emu_start(0xc267c8,0x50f0000,count=200000)
 except Exception:
  print("HISTORY",[hex(x) for x in history]);print("FAILED PC",hex(u.reg_read(r.UC_ARM64_REG_PC)),"LR",hex(u.reg_read(r.UC_ARM64_REG_LR)));raise
 if service!='none':
  assert u.reg_read(r.UC_ARM64_REG_PC)==0x50f0000 and 'native_async_provider' not in calls
  calls.append('pending_name_has_not_created')
  u.mem_write(0x500f000+0x2a,b'\1');u.reg_write(r.UC_ARM64_REG_X0,panel);u.reg_write(r.UC_ARM64_REG_SP,0x50ff000);u.reg_write(r.UC_ARM64_REG_LR,0x50f0000);u.emu_start(0xc24cc8,0x50f0000,count=200000)
 result['panel_name']=string(name).decode();result['calls']=calls;result['end']=hex(u.reg_read(r.UC_ARM64_REG_PC));return result
if __name__=='__main__':
 bridge='--baseline' not in sys.argv;checks=0
 for route in ('PC','Enter','Switch','Confirmed'):
  result=run(route,bridge=bridge);print(route,result)
  assert result['request_name']=='PruebaPC',result
  assert result['name']==result['panel_name']=='PruebaPC' and result['class']==3 and result['version']==2,result
  assert result['calls'].count('native_async_provider')==1,result
  assert result['calls'].count('host_A4_adapter')==int(bridge and route in ('PC','Enter')),result
  checks+=4
 for text in ('','A','A!','Muñeco','NombreDemasiadoLargo'):
  result=run('PC',text,bridge=bridge);assert result.get('validation_popup') and 'native_async_provider' not in result['calls'],result;checks+=1;print('PASS nombre rechazado nativamente',repr(text))
 for version in (1,2,3):
  result=run('PC',version=version,bridge=bridge);assert result['version']==version and result['request_name']=='PruebaPC';checks+=1
 for route in ('PC','Enter'):
  result=run(route,bridge=bridge,service='pending');assert result['request_name']=='PruebaPC' and result['calls'].count('native_async_provider')==1 and 'pending_name_has_not_created' in result['calls'];checks+=1;print('PASS proveedor diferido→Update nativo',route,result)
 print('PASS',checks,': handler→string assign→validadores nativos→constructor offline→solicitud de proveedor. Servicio de nombre devuelve NULL (fallback nativo); no I/O ni ejecución del trabajo asíncrono ni GUI.')
