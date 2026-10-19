"""OBS26: AST/producto + dm_gate ensamblado real; callback readonly simulado, sin GUI."""
from pathlib import Path
import ast,hashlib,re,struct,subprocess,tempfile,sys
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as r
source=Path('dm/tools/patch_nso.py');original=Path('dm/host/tests/build/main.orig.bin').read_bytes()
assert hashlib.sha256(original).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
site=0x3b1cb4;word=0xa9ba6ffc;assert struct.unpack_from('<I',original,site)[0]==word
tree=ast.parse(source.read_text());block=next(n for n in tree.body if isinstance(n,ast.If) and any(isinstance(q,ast.Call) and isinstance(q.func,ast.Attribute) and q.func.attr=='append' and q.args and isinstance(q.args[0],ast.Tuple) and isinstance(q.args[0].elts[0],ast.Constant) and q.args[0].elts[0].value==site for q in ast.walk(n)))
assert ast.unparse(block.test)=='RULE_RUN and PRIVATE_GATES'
row=next(ast.literal_eval(q.args[0]) for q in ast.walk(block) if isinstance(q,ast.Call) and isinstance(q.func,ast.Attribute) and q.func.attr=='append' and q.args and isinstance(q.args[0],ast.Tuple) and isinstance(q.args[0].elts[0],ast.Constant) and q.args[0].elts[0].value==site)
assert row[:3]==(site,26,'a9ba6ffc')
assert 'OBS_PC_GRID_DRAW = 26, OBS_MAX = 27' in Path('dm/host/dm.h').read_text()
assert 'gate_obs_set(OBS_PC_GRID_DRAW, dm_ui_pc_grid_draw_obs)' in Path('dm/host/diag.c').read_text()
if len(sys.argv)>1:
 root=Path(sys.argv[1]);image=bytearray((root/'main.patched.bin').read_bytes());rows=[s.split('\t') for s in (root/'main.gates.tsv').read_text().splitlines() if s and not s.startswith('#')]
 ret=next(int(row[0],16) for row in rows if row[1:]==['obs','26',hex(site)])
 headers={s[2:].split('\t')[0]:int(s.split('\t')[1],0) for s in (root/'main.fixups.tsv').read_text().splitlines() if s.startswith('# ')};keypage=headers['KEY_PAGE_OFF'];keyindex=headers['KEY_KGATE_IDX'];mode='producto'
else:
 names={'site_obs','B','STP_PRE','LDP_POST','MRS_TPIDRRO','ADRP','LDRX','ADDR','BLR','dis','has18','emit_gate_call','emit_slot_addr','emit_key_addr'}
 functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in names]
 patch=bytearray();gates=[];patchoff=0x3400000;keypage=0x6000000;keyindex=0
 env={'struct':struct,'re':re,'md':Cs(CS_ARCH_ARM64,CS_MODE_ARM),'patch':patch,'fix':[],'gates':gates,'KEY_PAGE_OFF':keypage,'KEY_INDEX':{'KGATE':0}}
 env.update(here=lambda:patchoff+len(patch),emit=lambda w:patch.extend(struct.pack('<I',w)))
 exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),env)
 start=env['site_obs'](site,26,word);ret=gates[0][0];image=bytearray(original);image.extend(bytes(patchoff+len(patch)-len(image)));image[patchoff:]=patch;struct.pack_into('<I',image,site,env['B'](site,start));mode='AST exacto de producción'
 assert struct.unpack_from('<I',image,ret+4)[0]==word
print('OBS26',mode,'imagen',hashlib.sha256(image).hexdigest())
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
 for context in ('off','legacy','physical'):
  for flags in (0,0xa0000000,0xf0000000):
   u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,bytes(image))
   for p,n in ((0x6000000,0x10000),(0x7000000,0x10000),(0x8000000,0x1000),(asm_base,(len(code)+4095)&~4095)):
    if p>=len(image):u.mem_map(p,n)
   u.mem_write(asm_base,code);u.mem_write(keypage+8*keyindex,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',asm_base+gate));u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100)
   values=[0x1110000+i*16 for i in range(31)];vectors=[(i+1)*0x112233445566778899aabbccddeeff for i in range(32)]
   for i,v in enumerate(values):u.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),v)
   for i,v in enumerate(vectors):u.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),v)
   sp=0x700f000;u.reg_write(r.UC_ARM64_REG_SP,sp);u.reg_write(r.UC_ARM64_REG_NZCV,flags);u.reg_write(r.UC_ARM64_REG_FPCR,0);u.reg_write(r.UC_ARM64_REG_FPSR,0);calls=[]
   def host(cpu,at,size,data):
    if at!=asm_base+dispatch:return
    assert cpu.reg_read(r.UC_ARM64_REG_X0)==ret;stack=cpu.reg_read(r.UC_ARM64_REG_X1);saved=list(struct.unpack('<31Q',cpu.mem_read(stack,248)));expected=values[:];expected[30]=ret;assert saved==expected
    calls.append(context)
    # DispatcherC es sink readonly; se ensucian registros físicos para probar la restauración del ensamblador real.
    for i in range(18):cpu.reg_write(getattr(r,f'UC_ARM64_REG_X{i}'),0xdead000+i)
    for i in range(32):cpu.reg_write(getattr(r,f'UC_ARM64_REG_Q{i}'),0xbeef000+i)
    cpu.reg_write(r.UC_ARM64_REG_NZCV,0x50000000);cpu.reg_write(r.UC_ARM64_REG_FPCR,0x400000);cpu.reg_write(r.UC_ARM64_REG_FPSR,1);cpu.reg_write(r.UC_ARM64_REG_PC,at+4)
   u.hook_add(UC_HOOK_CODE,host);u.emu_start(site,site+4,count=1000)
   assert calls==[context] and u.reg_read(r.UC_ARM64_REG_PC)==site+4
   assert [u.reg_read(getattr(r,f'UC_ARM64_REG_X{i}')) for i in range(31)]==values
   assert [u.reg_read(getattr(r,f'UC_ARM64_REG_Q{i}')) for i in range(32)]==vectors
   assert u.reg_read(r.UC_ARM64_REG_SP)==sp-96 and struct.unpack('<QQ',u.mem_read(sp-96,16))==(values[28],values[27])
   assert u.reg_read(r.UC_ARM64_REG_NZCV)==flags and not u.reg_read(r.UC_ARM64_REG_FPCR) and not u.reg_read(r.UC_ARM64_REG_FPSR)
   checks+=1
 print('PASS',checks,'OBS26: prólogoSTP una vez, GPR0..30/SIMD0..31/NZCV/FPCR/FPSR/SP/LR restaurados; dm_gate ensamblado real. CallbackC sink readonly, off/legacy/contexto; cuerpoC separado, no GUI.')
