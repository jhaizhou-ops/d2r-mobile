"""A4: corte BL original→SVC, AST de producción y dm_gate real; dispatcher C doblado."""
from pathlib import Path
import ast,hashlib,struct,sys,subprocess,tempfile
from types import SimpleNamespace
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r
source=Path('dm/tools/patch_nso.py');original=Path('dm/host/tests/build/main.orig.bin').read_bytes()
assert hashlib.sha256(original).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
site=0xc26e0c
if len(sys.argv)>1:
 p=Path(sys.argv[1]);image=bytearray((p/'main.patched.bin').read_bytes())
 rows=[s.split('\t') for s in (p/'main.gates.tsv').read_text().splitlines() if s and not s.startswith('#')]
 ret=next(int(row[0],16) for row in rows if row[1:]==['svc','164',hex(site)])
 headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0) for line in (p/'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
 key_page=headers['KEY_PAGE_OFF'];key_index=headers['KEY_KGATE_IDX'];mode='producto'
else:
 tree=ast.parse(source.read_text());names={'B','STP_PRE','LDP_POST','MRS_TPIDRRO','ADRP','LDRX','ADDR','BLR','emit_gate_call','emit_slot_addr','emit_key_addr','site_gate'}
 functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in names]
 block=next(n for n in tree.body if isinstance(n,ast.If) and any(isinstance(q,ast.Constant) and q.value==site for q in ast.walk(n)))
 patch=bytearray();gates=[];cuts=[];patch_off=0x3400000;key_page=0x6000000;key_index=0
 env={'struct':struct,'IMG':original,'RULE_RUN':True,'PRIVATE_GATES':True,'gates':gates,'cuts':cuts,'KEY_PAGE_OFF':key_page,'KEY_INDEX':{'KGATE':0},'fix':[],'patch':patch}
 env.update(here=lambda:patch_off+len(patch),emit=lambda word:patch.extend(struct.pack('<I',word)))
 exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),env)
 exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),env)
 assert len(cuts)==1 and cuts[0][:3]==(site,'24faff97','811400d4')
 start=env['site_gate'](SimpleNamespace(address=site),'svc',0xa4)
 image=bytearray(original);image.extend(bytes(patch_off+len(patch)-len(image)));image[patch_off:]=patch;struct.pack_into('<I',image,site,env['B'](site,start));ret=gates[0][0];mode='bloque AST+site_gate de producción, sin PRODUCT'
 for changed in (0xc267c8,0xc26e08,site,0xc26e10):
  altered=bytearray(original);altered[changed]^=1;trial=env.copy();trial['IMG']=bytes(altered)
  try:exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),trial)
  except AssertionError:pass
  else:raise AssertionError('No rechazó cambio original '+hex(changed))
 for rules,private in ((False,True),(True,False)):
  trial=env.copy();trial.update(RULE_RUN=rules,PRIVATE_GATES=private);count=len(cuts);exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),trial);assert len(cuts)==count
 print('PASS6: cuatro preimágenes rechazadas y dos dominios públicos sin gate')
assert 'SVC_HOST_PC_CREATE_COMMIT = 0xa4' in Path('dm/host/hle.h').read_text()
assert 'case SVC_HOST_PC_CREATE_COMMIT:' in Path('dm/host/hle_tact.c').read_text()
print('A4',mode,'SHA256 imagen',hashlib.sha256(image).hexdigest())
with tempfile.TemporaryDirectory(prefix='d2r-create-abi-') as folder:
 obj=Path(folder)/'entry.o';subprocess.run(['clang','-c','-o',str(obj),'dm/host/entry.S'],check=True);macho=obj.read_bytes()
 assert struct.unpack_from('<I',macho)[0]==0xfeedfacf
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
  if info&(1<<27) and names[info&0xffffff]=='_gate_dispatch':targets.append(a)
 assert len(targets)==1;dispatch=targets[0];assert struct.unpack_from('<I',code,dispatch)[0]&0xfc000000==0x94000000
 asm_base=0xa000000;checks=0
 for context in ('pc-click','pc-enter','confirmed','legacy'):
  for flags in (0,0xa0000000,0xf0000000):
   u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,bytes(image))
   for p,n in ((0x6000000,0x10000),(0x7000000,0x10000),(0x8000000,0x1000),(asm_base,(len(code)+4095)&~4095)):
    if p>=len(image):u.mem_map(p,n)
   u.mem_write(asm_base,code);u.mem_write(key_page+8*key_index,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',asm_base+gate));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
   values=[0x1110000+i*16 for i in range(31)];vectors=[(i+1)*0x112233445566778899aabbccddeeff for i in range(32)]
   for i,v in enumerate(values):u.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),v)
   for i,v in enumerate(vectors):u.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),v)
   values[0]=values[19]=0x9000000;values[20]=0x9000800;u.mem_map(0x9000000,0x1000)
   for i in (0,19,20):u.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),values[i])
   sp=0x700f000;u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags);u.reg_write(r.UC_ARM64_REG_FPCR,0);u.reg_write(r.UC_ARM64_REG_FPSR,0);calls=[]
   def host(cpu,at,size,data):
    if at!=asm_base+dispatch:return
    assert cpu.reg_read(r.UC_ARM64_REG_X0)==ret;stack=cpu.reg_read(r.UC_ARM64_REG_X1);saved=list(struct.unpack('<31Q',cpu.mem_read(stack,248)));expected=values[:];expected[30]=ret;assert saved==expected
    calls.append(context)
    # Dispatcher C doblado; ensucia registros físicos para contrastar el restore real de dm_gate.
    for i in range(18):cpu.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),0xdead000+i)
    for i in range(32):cpu.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),0xbeef000+i)
    cpu.reg_write(r.UC_ARM64_REG_NZCV,0x50000000);cpu.reg_write(r.UC_ARM64_REG_FPCR,0x400000);cpu.reg_write(r.UC_ARM64_REG_FPSR,1);cpu.reg_write(r.UC_ARM64_REG_PC,at+4)
   def stop(cpu,at,size,data):
    if at==site+4:cpu.emu_stop()
   u.hook_add(UC_HOOK_CODE,host);u.hook_add(UC_HOOK_CODE,stop);u.emu_start(site,0xb000000,count=1000)
   assert calls==[context] and u.reg_read(r.UC_ARM64_REG_PC)==site+4
   assert [u.reg_read(getattr(r,f'UC_ARM64_REG_X{i}')) for i in range(31)]==values
   assert [u.reg_read(getattr(r,f'UC_ARM64_REG_Q{i}')) for i in range(32)]==vectors
   assert u.reg_read(r.UC_ARM64_REG_SP)==sp
   assert u.reg_read(r.UC_ARM64_REG_NZCV)==flags and not u.reg_read(r.UC_ARM64_REG_FPCR) and not u.reg_read(r.UC_ARM64_REG_FPSR)
   checks+=1
 print('PASS',checks,'A4: GPR0..30/SIMD0..31/NZCV/FPCR/FPSR/SP/LR intactos, selfX0/X19 y messageX20 completos; retorna después del BL adaptado. dm_gate real, dispatcherC sink; constructor original probado aparte, sin GUI.')
