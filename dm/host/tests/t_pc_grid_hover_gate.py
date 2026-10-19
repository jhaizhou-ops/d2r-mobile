"""A3 Grid hover 门，生产 AST 和真实 dm_gate 汇编；C 分派为测试替身。
A3 Grid-hover gate, production AST and real dm_gate assembly; C dispatch is a test double.
"""
from pathlib import Path
import os
import ast,hashlib,struct,sys,subprocess,tempfile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r
source=Path('dm/tools/patch_nso.py');original=Path(os.environ.get('DM_MAIN_ORIGINAL','dm/host/tests/build/main.orig.bin')).read_bytes()
assert hashlib.sha256(original).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
site=0xabcca4;old=site+4;target=0xabccd8;regs=[getattr(r,f'UC_ARM64_REG_X{i}') for i in range(31)]
if len(sys.argv)>1:
 p=Path(sys.argv[1]);image=bytearray((p/'main.patched.bin').read_bytes())
 rows=[s.split('\t') for s in (p/'main.gates.tsv').read_text().splitlines() if s and not s.startswith('#')]
 ret=next(int(row[0],16) for row in rows if row[1:]==['svc','163',hex(site)])
 headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0) for line in (p/'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
 key_page=headers['KEY_PAGE_OFF'];key_index=headers['KEY_KGATE_IDX'];mode='producto'
else:
 # 执行相同的生产 AST，不复制其指令。 | Execute the same production AST rather than a replica of its instructions.
 tree=ast.parse(source.read_text());names={'B','STP_PRE','LDP_POST','MRS_TPIDRRO','ADRP','LDRX','ADDR','BLR','emit_gate_call','emit_slot_addr','emit_key_addr'}
 functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in names]
 block=next(n for n in tree.body if isinstance(n,ast.If) and any(isinstance(q,ast.Constant) and q.value=='pc_grid_hover' for q in ast.walk(n)))
 patch=bytearray();gates=[];changes=[];patch_off=0x3400000;key_page=0x6000000;key_index=0
 env={'struct':struct,'IMG':original,'RULE_RUN':True,'PRIVATE_GATES':True,'TEXT_END':0x1b80000,'kinds':{},'windows':[],'gates':gates,'cuts':changes,'KEY_PAGE_OFF':key_page,'KEY_INDEX':{'KGATE':0},'fix':[],'patch':patch}
 env.update(here=lambda:patch_off+len(patch),emit=lambda word:patch.extend(struct.pack('<I',word)),stat=lambda *_:None)
 def put(at,to):changes.append((at,to))
 env['put_branch']=put
 exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),env)
 exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),env)
 image=bytearray(original);image.extend(bytes(patch_off+len(patch)-len(image)));image[patch_off:]=patch
 for item in changes:
  if len(item)==2:struct.pack_into('<I',image,item[0],env['B'](*item))
 ret=gates[0][0];mode='bloque AST de producción, sin PRODUCT'
assert struct.unpack_from('<I',original,site)[0]==0xa9177e7f
assert "SVC_HOST_PC_GRID_HOVER = 0xa3" in Path('dm/host/hle.h').read_text()
assert "case SVC_HOST_PC_GRID_HOVER:" in Path('dm/host/hle_tact.c').read_text()
if len(sys.argv)==1:
 rejected=0
 for changed in (site,0xabcc50,0xabcc88,0xabcca8,0xabccd4,0xabccd8):
  mutated=bytearray(original);mutated[changed]^=1;trial=env.copy();trial['IMG']=bytes(mutated)
  try:exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),trial)
  except AssertionError:rejected+=1
 assert rejected==6
 for rule,private in ((False,True),(True,False)):
  trial=env.copy();trial.update(RULE_RUN=rule,PRIVATE_GATES=private);size=len(patch);count=len(gates)
  exec(compile(ast.Module(body=[block],type_ignores=[]),str(source),'exec'),trial)
  assert (len(patch),len(gates))==(size,count)
 print('PASS8 guardasAST: seis mutaciones originales rechazadas y dos perfiles públicos sin gate')
print('A3',mode,'SHA256 imagen',hashlib.sha256(image).hexdigest())
with tempfile.TemporaryDirectory(prefix='d2r-grid-abi-') as folder:
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
 for context,allow in (('off',0),('legacy',0),('physical',1)):
  for flags in (0,0xa0000000,0xf0000000):
   u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,bytes(image))
   for p,n in ((0x6000000,0x10000),(0x7000000,0x10000),(0x8000000,0x1000),(asm_base,(len(code)+4095)&~4095)):
    if p>=len(image):u.mem_map(p,n)
   u.mem_write(asm_base,code);u.mem_write(key_page+8*key_index,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',asm_base+gate));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
   values=[0x1110000+i*16 for i in range(31)];vectors=[(i+1)*0x112233445566778899aabbccddeeff for i in range(32)]
   for i,v in enumerate(values):u.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),v)
   for i,v in enumerate(vectors):u.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),v)
   values[19]=0x9000000;u.mem_map(0x9000000,0x1000);u.mem_write(values[19]+0x170,struct.pack('<QQ',0x123456,0x654321));u.reg_write(r.UC_ARM64_REG_X19,values[19]);sp=0x700f000;u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags);u.reg_write(r.UC_ARM64_REG_FPCR,0);u.reg_write(r.UC_ARM64_REG_FPSR,0);calls=[]
   def host(cpu,at,size,data):
    if at!=asm_base+dispatch:return
    assert cpu.reg_read(r.UC_ARM64_REG_X0)==ret;stack=cpu.reg_read(r.UC_ARM64_REG_X1);saved=list(struct.unpack('<31Q',cpu.mem_read(stack,248)));expected=values[:];expected[30]=ret;assert saved==expected
    calls.append(context);cpu.mem_write(stack+8*8,struct.pack('<Q',allow))
    # 分派替身只决定 savedx8；破坏物理寄存器以检查真实恢复。 | The dispatcher double changes savedx8 only; clobber physical registers to check real restoration.
    for i in range(18):cpu.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),0xdead000+i)
    for i in range(32):cpu.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),0xbeef000+i)
    cpu.reg_write(r.UC_ARM64_REG_NZCV,0x50000000);cpu.reg_write(r.UC_ARM64_REG_FPCR,0x400000);cpu.reg_write(r.UC_ARM64_REG_FPSR,1);cpu.reg_write(r.UC_ARM64_REG_PC,at+4)
   def stop(cpu,at,size,data):
    if at in (site+4,target):cpu.emu_stop()
   u.hook_add(UC_HOOK_CODE,host);u.hook_add(UC_HOOK_CODE,stop);u.emu_start(site,0xb000000,count=1000)
   assert calls==[context] and u.reg_read(r.UC_ARM64_REG_PC)==(target if allow else site+4)
   assert [u.reg_read(getattr(r,f'UC_ARM64_REG_X{i}')) for i in range(31)]==values
   assert [u.reg_read(getattr(r,f'UC_ARM64_REG_Q{i}')) for i in range(32)]==vectors
   assert u.reg_read(r.UC_ARM64_REG_SP)==sp and struct.unpack('<QQ',u.mem_read(values[19]+0x170,16))==((0x123456,0x654321) if allow else (0,0))
   assert u.reg_read(r.UC_ARM64_REG_NZCV)==flags and not u.reg_read(r.UC_ARM64_REG_FPCR) and not u.reg_read(r.UC_ARM64_REG_FPSR)
   checks+=1
 print('PASS',checks,'A3: GPR0..30/SIMD0..31/NZCV/FPCR/FPSR/SP/LR intactos; replaySTP o bypassClear completo, dm_gate ensamblado real. DispatcherC sink, sin GUI.')
