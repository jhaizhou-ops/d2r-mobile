"""A5/A6: bloques de producción y dm_gate real; callbacks C y getters son dobles.
Comprueba ASLR, registros/flags, RET C0 y ambos BL originales del fallback HUD.
"""
from pathlib import Path
import ast,hashlib,struct,sys,subprocess,tempfile,re
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r

source=Path('dm/tools/patch_nso.py');original=Path('dm/host/tests/build/main.orig.bin').read_bytes()
assert hashlib.sha256(original).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
sites={'click':(0x3e4760,0xa5),'hud':(0x3e2f6c,0xa6)};guards=0
if len(sys.argv)>1:
 out=Path(sys.argv[1]);image=bytearray((out/'main.patched.bin').read_bytes())
 rows=[s.split('\t')for s in(out/'main.gates.tsv').read_text().splitlines()if s and not s.startswith('#')]
 returns={name:next(int(row[0],16)for row in rows if row[1:]==['svc',str(number),hex(site)])for name,(site,number)in sites.items()}
 headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0)for line in(out/'main.fixups.tsv').read_text().splitlines()if line.startswith('# ')}
 key_page=headers['KEY_PAGE_OFF'];key_index=headers['KEY_KGATE_IDX'];mode='producto'
else:
 tree=ast.parse(source.read_text());names={'B','STP_PRE','LDP_POST','MRS_TPIDRRO','ADRP','LDRX','ADDR','BLR','emit_gate_call','emit_slot_addr','emit_key_addr'}
 functions=[n for n in tree.body if isinstance(n,ast.FunctionDef)and n.name in names]
 blocks={name:next((n for n in tree.body if isinstance(n,ast.If)and any(isinstance(q,ast.Constant)and q.value==tag for q in ast.walk(n))),None)for name,tag in [('click','pc_skill_click'),('hud','pc_skill_hud')]}
 assert all(blocks.values()),'Faltan los gates de producción A5/A6'
 patch=bytearray();gates=[];changes=[];patch_off=0x3400000;key_page=0x6000000;key_index=0
 env={'struct':struct,'IMG':original,'RULE_RUN':True,'PRIVATE_GATES':True,'TEXT_END':0x1b80000,'kinds':{},'windows':[],'gates':gates,'cuts':[],'KEY_PAGE_OFF':key_page,'KEY_INDEX':{'KGATE':0},'fix':[],'patch':patch}
 env.update(here=lambda:patch_off+len(patch),emit=lambda word:patch.extend(struct.pack('<I',word)),stat=lambda *_:None,put_branch=lambda at,to:changes.append((at,to)))
 exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),env)
 for block in blocks.values():exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),env)
 image=bytearray(original);image.extend(bytes(patch_off+len(patch)-len(image)));image[patch_off:]=patch
 for at,to in changes:struct.pack_into('<I',image,at,env['B'](at,to))
 returns={name:next(row[0]for row in gates if row[1:]==('svc',number,site))for name,(site,number)in sites.items()}
 for name,offsets in [('click',(0x3e4594,0x3e4750,0x3e4760)),('hud',(0x3e2dd4,0x3e2f6c,0x3e2f70,0x3e2f7c,0x3e2f80))]:
  for changed in offsets:
   altered=bytearray(original);altered[changed]^=1;trial=env.copy();trial['IMG']=bytes(altered)
   try:exec(compile(ast.Module(body=[blocks[name]],type_ignores=[]),str(source),'exec'),trial)
   except AssertionError:guards+=1
   else:raise AssertionError('Preimagen no rechazada '+hex(changed))
 for rule,private in ((False,True),(True,False)):
  trial=env.copy();trial.update(RULE_RUN=rule,PRIVATE_GATES=private);size=len(patch);count=len(gates)
  for block in blocks.values():exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),trial)
  assert (len(patch),len(gates))==(size,count);guards+=1
 mode='AST de producción'
 print('PASS',guards,'guardas AST: ocho preimágenes y dos perfiles públicos')
for name,number in [('CLICK',0xa5),('HUD',0xa6)]:
 assert f'SVC_HOST_PC_SKILL_{name} = {hex(number)}'in Path('dm/host/hle.h').read_text()
 assert f'case SVC_HOST_PC_SKILL_{name}:'in Path('dm/host/hle_tact.c').read_text()
registry=re.findall(r'(SVC_HOST_[A-Z0-9_]+)\s*=\s*(0x[0-9a-f]+)',Path('dm/host/hle.h').read_text())
assert len(registry)==len({int(value,16)for _,value in registry}),'Colisión en el registro SVC host'
print('A5/A6',mode,'SHA256 imagen',hashlib.sha256(image).hexdigest())

with tempfile.TemporaryDirectory(prefix='d2r-skill-gates-')as folder:
 obj=Path(folder)/'entry.o';subprocess.run(['clang','-c','-o',str(obj),'dm/host/entry.S'],check=True);macho=obj.read_bytes();assert struct.unpack_from('<I',macho)[0]==0xfeedfacf
 at=32;text=None;sym=None
 for _ in range(struct.unpack_from('<I',macho,16)[0]):
  cmd,size=struct.unpack_from('<II',macho,at)
  if cmd==0x19:
   for j in range(struct.unpack_from('<I',macho,at+64)[0]):
    p=at+72+80*j
    if macho[p:p+16].rstrip(b'\0')==b'__text':text=struct.unpack_from('<QQIIII',macho,p+32)
  elif cmd==2:sym=struct.unpack_from('<IIII',macho,at+8)
  at+=size
 assert text and sym
 syms={};names=[]
 for i in range(sym[1]):
  st,kind,section,desc,value=struct.unpack_from('<IBBHQ',macho,sym[0]+i*16);p=sym[2]+st;name=macho[p:macho.index(b'\0',p)].decode();syms[name]=value;names.append(name)
 addr,size,off,align,reloff,nreloc=text;code=macho[off:off+size];gate=syms['_dm_gate']-addr
 targets=[]
 for i in range(nreloc):
  a,info=struct.unpack_from('<iI',macho,reloff+8*i)
  if info&(1<<27)and names[info&0xffffff]=='_gate_dispatch':targets.append(a)
 assert len(targets)==1;dispatch=targets[0];asm_base=0xa000000;checks=0
 for base in (0,0x7000000000):
  for name,(site,number)in sites.items():
   contexts=[('off',0),('legacy',0),('pc-left',1),('pc-right',1),('picker',0)]if name=='hud'else [('off',0),('legacy',0),('pc-left',1),('pc-right',1),('picker',1)]
   for context,allow in contexts:
    for flags in (0,0xa0000000,0xf0000000):
     u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,(len(image)+4095)&~4095);u.mem_write(base,bytes(image))
     for p,n in ((base+key_page,0x10000),(0x7000000,0x10000),(0x8000000,0x1000),(0x9000000,0x10000),(asm_base,(len(code)+4095)&~4095)):
      if not base<=p<base+len(image):u.mem_map(p,n)
     u.mem_write(asm_base,code);u.mem_write(base+key_page+8*key_index,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',asm_base+gate));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
     values=[0x1110000+i*16 for i in range(31)];values[0]=values[19]=0x9000000;values[23]=5;values[30]=0x900f000
     vectors=[(i+1)*0x112233445566778899aabbccddeeff for i in range(32)]
     for i,v in enumerate(values):u.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),v)
     for i,v in enumerate(vectors):u.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),v)
     sp=0x700f000;u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags);u.reg_write(r.UC_ARM64_REG_FPCR,0x400000);u.reg_write(r.UC_ARM64_REG_FPSR,1)
     calls=[];native=[];ret=base+returns[name]
     def host(cpu,at,size,data):
      if at==asm_base+dispatch:
       assert cpu.reg_read(r.UC_ARM64_REG_X0)==ret;stack=cpu.reg_read(r.UC_ARM64_REG_X1);saved=list(struct.unpack('<31Q',cpu.mem_read(stack,248)));expected=values[:];expected[30]=ret;assert saved==expected
       calls.append(context)
       if name=='hud':
        cpu.mem_write(stack+8*8,struct.pack('<Q',allow))
        if allow:cpu.mem_write(stack,struct.pack('<Q',0xffffffff));cpu.mem_write(stack+21*8,struct.pack('<Q',42))
       for i in range(31):
        if i!=18:cpu.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),0xdead000+i)
       for i in range(32):cpu.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),0xbeef000+i)
       cpu.reg_write(r.UC_ARM64_REG_NZCV,0x50000000);cpu.reg_write(r.UC_ARM64_REG_FPCR,0x800000);cpu.reg_write(r.UC_ARM64_REG_FPSR,0);cpu.reg_write(r.UC_ARM64_REG_PC,at+4)
      elif at in(base+0x43ebb0,base+0x43ef10):
       assert cpu.reg_read(r.UC_ARM64_REG_W0)==5
       native.append((at-base,cpu.reg_read(r.UC_ARM64_REG_LR)-base));cpu.reg_write(r.UC_ARM64_REG_W0,42 if at==base+0x43ebb0 else 0xffffffff);cpu.reg_write(r.UC_ARM64_REG_PC,cpu.reg_read(r.UC_ARM64_REG_LR))
      elif at==(values[30]if name=='click'else base+0x3e2f80):cpu.emu_stop()
     u.hook_add(UC_HOOK_CODE,host);u.emu_start(base+site,0xb000000000,count=1000)
     expected=values[:]
     if name=='hud':
      expected[0]=0xffffffff;expected[21]=42
      if not allow:expected[30]=base+0x3e2f80
      assert native==([]if allow else[(0x43ebb0,0x3e2f74),(0x43ef10,0x3e2f80)])
     else:assert not native
     assert calls==[context]and u.reg_read(r.UC_ARM64_REG_PC)==(values[30]if name=='click'else base+0x3e2f80)
     assert [u.reg_read(getattr(r,f'UC_ARM64_REG_X{i}'))for i in range(31)]==expected,(name,context)
     assert [u.reg_read(getattr(r,f'UC_ARM64_REG_Q{i}'))for i in range(32)]==vectors
     assert u.reg_read(r.UC_ARM64_REG_SP)==sp and u.reg_read(r.UC_ARM64_REG_NZCV)==flags and u.reg_read(r.UC_ARM64_REG_FPCR)==0x400000 and u.reg_read(r.UC_ARM64_REG_FPSR)==1
     checks+=1
 print('PASS',checks,'A5/A6: dos bases ASLR, GPR/SIMD/NZCV/FPCR/FPSR/SP, RET C0 y ambos BL/LR originales del fallback; x18 reservado sin clobber, host/getters doblados, dm_gate real, sin GUI.')
