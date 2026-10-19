"""A7/A8: bloques de producción y dm_gate real; decisión host doblada.
Append original completo contra baseline, ASLR, GPR/SIMD/flags y LR exactos.
"""
from pathlib import Path
import ast,hashlib,struct,sys,subprocess,tempfile,re
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r

source=Path('dm/tools/patch_nso.py');original=Path('dm/host/tests/build/main.orig.bin').read_bytes()
assert hashlib.sha256(original).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
sites={'candidate':(0xc065a4,0xa7),'common0':(0xc068f4,0xa8),'common1':(0xc06908,0xa8),'common2':(0xc0691c,0xa8)};guards=0
if len(sys.argv)>1:
 out=Path(sys.argv[1]);image=bytearray((out/'main.patched.bin').read_bytes())
 rows=[s.split('\t')for s in(out/'main.gates.tsv').read_text().splitlines()if s and not s.startswith('#')]
 returns={name:next(int(row[0],16)for row in rows if row[1:]==['svc',str(number),hex(site)])for name,(site,number)in sites.items()}
 headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0)for line in(out/'main.fixups.tsv').read_text().splitlines()if line.startswith('# ')}
 key_page=headers['KEY_PAGE_OFF'];key_index=headers['KEY_KGATE_IDX'];mode='producto'
else:
 tree=ast.parse(source.read_text());names={'B','ADDI','STP_PRE','LDP_POST','MRS_TPIDRRO','ADRP','LDRX','ADDR','BLR','emit_gate_call','emit_slot_addr','emit_key_addr'}
 functions=[n for n in tree.body if isinstance(n,ast.FunctionDef)and n.name in names]
 blocks={name:next((n for n in tree.body if isinstance(n,ast.If)and any(isinstance(q,ast.Constant)and q.value==tag for q in ast.walk(n))),None)for name,tag in [('candidate','pc_skill_candidate'),('common','pc_skill_common')]}
 assert all(blocks.values()),'Faltan los gates de producción A7/A8'
 patch=bytearray();gates=[];changes=[];patch_off=0x3400000;key_page=0x6000000;key_index=0
 env={'struct':struct,'IMG':original,'RULE_RUN':True,'PRIVATE_GATES':True,'TEXT_END':0x1b80000,'kinds':{},'windows':[],'gates':gates,'cuts':[],'KEY_PAGE_OFF':key_page,'KEY_INDEX':{'KGATE':0},'fix':[],'patch':patch}
 env.update(here=lambda:patch_off+len(patch),emit=lambda word:patch.extend(struct.pack('<I',word)),stat=lambda *_:None,put_branch=lambda at,to:changes.append((at,to)))
 exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),env)
 for block in blocks.values():exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),env)
 image=bytearray(original);image.extend(bytes(patch_off+len(patch)-len(image)));image[patch_off:]=patch
 for at,to in changes:struct.pack_into('<I',image,at,env['B'](at,to))
 returns={name:next(row[0]for row in gates if row[1:]==('svc',number,site))for name,(site,number)in sites.items()}
 for name,offsets in [('candidate',tuple(range(0xc06378,0xc0639c,4))+tuple(range(0xc064dc,0xc064ec,4))+tuple(range(0xc065a4,0xc065c0,4))+tuple(range(0xc066b0,0xc066c0,4))),('common',tuple(range(0xc06378,0xc0639c,4))+tuple(range(0xc064dc,0xc064ec,4))+tuple(range(0xc068c8,0xc06920,4))+tuple(range(0xc08cc8,0xc08cd8,4)))]:
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
 print('PASS',guards,'guardas AST: 63 palabras de preimagen y dos perfiles públicos')
for name,number in [('CANDIDATE',0xa7),('COMMON',0xa8)]:
 assert f'SVC_HOST_PC_SKILL_{name} = {hex(number)}'in Path('dm/host/hle.h').read_text()
 assert f'case SVC_HOST_PC_SKILL_{name}: {{ int dm_ui_pc_skill_{name.lower()}(uint64_t *);regs[8]=dm_ui_pc_skill_{name.lower()}(regs)!=0;return 1; }}'in Path('dm/host/hle_tact.c').read_text()
registry=re.findall(r'(SVC_HOST_[A-Z0-9_]+)\s*=\s*(0x[0-9a-f]+)',Path('dm/host/hle.h').read_text())
assert len(registry)==len({int(value,16)for _,value in registry}),'Colisión en el registro SVC host'
print('A7/A8',mode,'SHA256 imagen',hashlib.sha256(image).hexdigest())

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
   for context,skip,side in [('off',0,1),('legacy',0,2),('pc-valid',0,1),('pc-left-reject',1,1),('pc-right-reject',1,2)]:
    for flags in (0,0xa0000000,0xf0000000):
     values=[0x1110000+i*16 for i in range(31)];values[0]=base+0x2f12d20;values[1]=0x9006000;values[8]=0x9002000;values[20]=0x9003000;values[23]=0x9001000;values[29]=0x700f200;values[30]=0x900f000
     vectors=[(i+1)*0x112233445566778899aabbccddeeff for i in range(32)];sp=0x700f000
     def machine(baseline):
      u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,(len(image)+4095)&~4095);u.mem_write(base,bytes(image))
      if baseline:u.mem_write(base+site,original[site:site+4])
      for p,n in ((base+key_page,0x10000),(0x7000000,0x10000),(0x8000000,0x1000),(0x9000000,0x10000),(asm_base,(len(code)+4095)&~4095)):
       if not base<=p<base+len(image):u.mem_map(p,n)
      u.mem_write(asm_base,code);u.mem_write(base+key_page+8*key_index,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',asm_base+gate));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
      u.mem_write(sp+0x84,struct.pack('<I',side));u.mem_write(values[0],struct.pack('<3Q',0x9004000,0,8));u.mem_write(values[1],struct.pack('<I',357))
      for i,v in enumerate(values):u.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),v)
      for i,v in enumerate(vectors):u.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),v)
      u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags);u.reg_write(r.UC_ARM64_REG_FPCR,0x400000);u.reg_write(r.UC_ARM64_REG_FPSR,1)
      return u
     def state(u):
      return ([u.reg_read(getattr(r,f'UC_ARM64_REG_X{i}'))for i in range(31)],[u.reg_read(getattr(r,f'UC_ARM64_REG_Q{i}'))for i in range(32)],u.reg_read(r.UC_ARM64_REG_SP),u.reg_read(r.UC_ARM64_REG_NZCV),u.reg_read(r.UC_ARM64_REG_FPCR),u.reg_read(r.UC_ARM64_REG_FPSR))
     u=machine(False);calls=[];appends=[];ret=base+returns[name]
     target=base+(0xc066b0 if skip else site+4)if name=='candidate'else base+site+4
     def host(cpu,at,size,data):
      if at==asm_base+dispatch:
       assert cpu.reg_read(r.UC_ARM64_REG_X0)==ret;stack=cpu.reg_read(r.UC_ARM64_REG_X1);saved=list(struct.unpack('<31Q',cpu.mem_read(stack,248)));expected=values[:];expected[30]=ret;assert saved==expected
       assert struct.unpack('<I',cpu.mem_read(saved[29]-0x17c,4))[0]==side
       calls.append(context);cpu.mem_write(stack+8*8,struct.pack('<Q',skip))
       for i in range(31):
        if i!=18:cpu.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),0xdead000+i)
       for i in range(32):cpu.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),0xbeef000+i)
       cpu.reg_write(r.UC_ARM64_REG_NZCV,0x50000000);cpu.reg_write(r.UC_ARM64_REG_FPCR,0x800000);cpu.reg_write(r.UC_ARM64_REG_FPSR,0);cpu.reg_write(r.UC_ARM64_REG_PC,at+4)
      elif at==base+0xc08cc8:
       assert cpu.reg_read(r.UC_ARM64_REG_LR)==base+site+4
       assert cpu.reg_read(r.UC_ARM64_REG_X0)==values[0]and cpu.reg_read(r.UC_ARM64_REG_X1)==values[1]
       appends.append(at)
      elif at==target:cpu.emu_stop()
     u.hook_add(UC_HOOK_CODE,host);u.emu_start(base+site,0xb000000000,count=2000)
     assert u.reg_read(r.UC_ARM64_REG_PC)==target and calls==[context]
     assert len(appends)==(0 if name=='candidate'or skip else 1)
     if not skip:
      baseline=machine(True)
      def stop_original(cpu,at,size,data):
       if at==base+site+4:cpu.emu_stop()
      baseline.hook_add(UC_HOOK_CODE,stop_original);baseline.emu_start(base+site,0xb000000000,count=2000)
      assert state(u)==state(baseline),(name,context,'baseline')
      assert u.mem_read(values[0],24)==baseline.mem_read(values[0],24)
      assert u.mem_read(0x9004000,32)==baseline.mem_read(0x9004000,32)
     else:
      expected=values[:]
      if name!='candidate':expected[30]=base+site+4
      assert state(u)==(expected,vectors,sp,flags,0x400000,1),(name,context)
      assert struct.unpack('<3Q',u.mem_read(values[0],24))==(0x9004000,0,8)
     if name!='candidate':
      assert struct.unpack('<Q',u.mem_read(values[0]+8,8))[0]==int(not skip)
      assert struct.unpack('<I',u.mem_read(0x9004000,4))[0]==(0 if skip else 357)
     checks+=1
 print('PASS',checks,'A7/A8: dos bases ASLR, GPR/SIMD/NZCV/FPCR/FPSR/SP, lado FP−17c, omisión sin writes y append original completo equivalente a BL baseline con LR exacto; host decisión doblada, dm_gate real, sin GUI.')
