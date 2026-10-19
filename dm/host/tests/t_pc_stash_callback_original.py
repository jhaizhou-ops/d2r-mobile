"""Parser tipo6 + Button Down/Up originales: callback Personal PC y ClosePanel sin coincidencia."""
from pathlib import Path
import hashlib,struct,json,sys
from unicorn import *
from unicorn import arm64_const as R
sys.path.insert(0,'dm/ios/tools')
from pc_desktop_layout import page_button
next_callback=page_button('TouchStashRight',921,1215,'Right')['fields']['onClickMessage']
previous_callback=page_button('TouchStashLeft',565,1215,'Left')['fields']['onClickMessage']
b=Path('dm/host/tests/build/main.orig.bin').read_bytes();assert hashlib.sha256(b).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
rel={}
for p in range(0x1b80000,0x1c80000,8):
 o,i,t=struct.unpack_from('<QQQ',b,p)
 if i==0x403:rel[o]=t
def fnv(s):
 h=0xcbf29ce484222325
 for c in s.encode():h=((h^c)*0x100000001b3)&((1<<64)-1)
 return h
records=[]
channels=[rel[p] for p in range(0x20bf1e8,0x20bf420,8)]
assert len(channels)==71
for text in ['PersonalStashMessage:NextPage',next_callback,previous_callback,'BankPanelMessage:SharedStashRight','PanelManager:ClosePanel:BankExpansionLayout']:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,0x4000000);u.mem_write(0,b);u.mem_map(0x5000000,0x200000);heap=0x5100000
 def w(p,v,f='Q'):u.mem_write(p,struct.pack('<'+f,v))
 def rd(p,f='Q'):return struct.unpack('<'+f,u.mem_read(p,struct.calcsize(f)))[0]
 def st(p):return bytes(u.mem_read(p,1024)).split(b'\0')[0].decode()
 for off,to in rel.items():w(off,to)
 def alloc(n):
  global heap
  p=heap;heap=(heap+n+15)&~15;assert heap<0x5200000;return p
 def string(s):
  p=alloc(len(s)+1);u.mem_write(p,s.encode()+b'\0');return p
 root,fm,button,desc,source,stop,regbucket,registry,meta,pm,bm=[0x5000000+i*0x1000 for i in range(11)]
 av,allocator,allocfn,freefn,sink=[0x500c000+i*0x1000 for i in range(5)]
 w(0x3211700,1,'B');w(0x32116f8,allocator);w(allocator,av);w(av+0x10,allocfn);w(av+0x28,freefn);w(0x2f05398,av);w(0x2f05398+0x0, av)
 # Registros y enums de una sola cubeta: el hash SDK no afecta a la búsqueda; sí comparaciones y lookup originales.
 def enum(obj,names):
  w(obj,0x20bc468);w(obj+0x28,1);bucket=alloc(8);w(obj+0x30,bucket);head=0
  for name in reversed(names):
   p=name if isinstance(name,int) else string(name);s=st(p);node=alloc(0x38);w(node,head);w(node+8,p);w(node+0x10,len(s));w(node+0x30,fnv(s));head=node
  w(bucket,head)
 enum(meta,channels);enum(pm,[rel[p] for p in range(0x20bf420,0x20bf460,8)]);enum(bm,['SharedStashLeft','SharedStashRight'])
 head=0
 for name,obj in [('BankPanelMessage',bm),('PanelManager',pm),('MessageChannels',meta)]:
  node=alloc(0x38);w(node,head);w(node+8,string(name));w(node+0x10,len(name));w(node+0x30,obj);head=node
 w(0x2f05220,1);w(0x2f05228,regbucket);w(regbucket,head)
 w(0x2f05458,root);w(root+0xd0,fm);w(fm+0x178,button);w(button,0x20b7b00);w(button+0x50,1,'B');w(button+0x51,1,'B');w(desc+8,6,'I');w(desc+0x10,0x558);u.mem_write(source,text.encode()+b'\0')
 emitted=[];found=[];root_run=False
 def ret(v=0):u.reg_write(R.UC_ARM64_REG_X0,v);u.reg_write(R.UC_ARM64_REG_PC,u.reg_read(R.UC_ARM64_REG_LR))
 def hook(cpu,at,size,data):
  x=[cpu.reg_read(getattr(R,f'UC_ARM64_REG_X{i}')) for i in range(4)]
  if at==0xb1cc5c and root_run:
   found.append(cpu.reg_read(R.UC_ARM64_REG_X19));cpu.emu_stop()
  elif at==0x1b76070:ret(len(st(x[0])))
  elif at in [0x1b76040,0x1b760c0]:u.mem_write(x[0],bytes(u.mem_read(x[1],x[2])));ret(x[0])
  elif at==0x1b76240:
   a=bytes(u.mem_read(x[0],x[2]));bb=bytes(u.mem_read(x[1],x[2]));ret(0 if a==bb else 1)
  elif at==0x1b6e1e0:w(x[0],0);ret()
  elif at==allocfn:ret(alloc(x[1]))
  elif at==freefn:ret()
  elif at==sink:
   p=x[1];arg=rd(p+16);emitted.append([rd(p),rd(p+8),st(arg) if arg else None]);ret()
  elif at in [0xb11bf0,0xb12f88]:pass
  elif at in [0xb7b108,0x35f120]:ret()
 u.hook_add(UC_HOOK_CODE,hook)
 def call(at,*args):
  for i,v in enumerate(args):u.reg_write(getattr(R,f'UC_ARM64_REG_X{i}'),v)
  u.reg_write(R.UC_ARM64_REG_SP,0x50f0000);u.reg_write(R.UC_ARM64_REG_LR,stop);u.emu_start(at,stop,count=100000)
  assert u.reg_read(R.UC_ARM64_REG_PC)==stop or (at==0xb1dc60 and found),hex(u.reg_read(R.UC_ARM64_REG_PC))
 call(0xb11ec4,desc,button,source,0);actual=[rd(button+0x558),rd(button+0x560)];expect=[fnv(text.split(':')[0]),fnv(text.split(':')[1])] if not text.startswith('Personal') else [0,0];assert actual==expect,(text,actual,expect)
 # El callback virtual real emite los campos y argumento producidos por el parser, sin preasignarlos.
 vt=alloc(0x40);w(root,vt);w(vt+0x28,sink)
 bank=0x500b000;page=alloc(0x100);arr=alloc(8);child=alloc(8);msg=alloc(24);payload=alloc(24)
 w(bank,0x2089210);w(page,0x20be678);w(bank+0x58,arr);w(bank+0x60,1);w(arr,page);w(page+0x58,child);w(page+0x60,1);w(child,button)
 for obj in [bank,page]:w(obj+0x50,1,'B');w(obj+0x51,1,'B')
 w(msg,fnv('InputMessage'));w(msg+16,payload);w(payload+4,781,'I');w(payload+8,1142,'I')
 for name in ['LeftMouseButtonDown','LeftMouseButtonUp']:
  w(root+0xb8,0,'B');w(msg+8,fnv(name));call(0x399858,bank,msg);assert rd(root+0xb8,'B')==1;assert rd(button+0x579,'B')==int(name.endswith('Down'))
 assert len(emitted)==(not text.startswith('Personal')),(text,emitted)
 if text.startswith('PanelManager'):
  arr=alloc(8);w(root+0x58,arr);w(root+0x60,1);w(arr,bank);w(bank,0x2089210);w(bank+8,string('BankExpansionLayout'));w(bank+0x50,1,'B');w(bank+0x51,1,'B')
  try:
   root_run=True;pre=bytes(u.mem_read(root,0x200))+bytes(u.mem_read(bank,0x200));call(0xb1dc60,root,button+0x558);post=bytes(u.mem_read(root,0x200))+bytes(u.mem_read(bank,0x200));assert bool(found)==text.endswith(':BankExpansionLayout');assert pre==post;print('PASS ROOT_CLOSE',text,'matched',len(found),'bank_visible',rd(bank+0x50,'B'),rd(bank+0x51,'B'),'state_unchanged',pre==post)
  except Exception as e:print('ROOTERROR',hex(u.reg_read(R.UC_ARM64_REG_PC)),hex(u.reg_read(R.UC_ARM64_REG_LR)),hex(u.reg_read(R.UC_ARM64_REG_X0)),e);raise
 if emitted:
  expected=-1 if text.endswith(':TouchPersonalStashPrevious') else 1 if text.endswith(':TouchPersonalStashNext') else 0
  records.append((emitted[0],expected))
 print('PASS parser b11ec4+registrob11bf0+enumlookupb12f88+callbackaaf6c4',text,actual,emitted)
if len(sys.argv)>1:
 with open(sys.argv[1],'wb') as out:
  for (cat,act,arg),expected in records:out.write(struct.pack('<QQ64si',cat,act,(arg or '').encode(),expected))
 assert len(records)==4
print('PASS5. Registrofixture de1cubeta; SDK strlen/memcpy/memcmp, hash irrelevante por máscara0 y allocator doblados. Canales71 y accionesPM8 de tablas originales; accionesBank son subconjuntofixture. CallbacksPersonal leídos de page_button producción. Sin JSONlexer ni GUI.')

print('ClosePanel positivo: se detiene en igualdad de nombre b1cc5c antes cast/OnClose. Los dos argumentos privados completan b1dc60 sin coincidencia ni cambio de bytes root/banco. Down/Up Bank→Widget→Button originales ejecutados después del parser. No lexerJSON, rasterización ni GUI.')
