"""Mutaciones en memoria: el verificador real rechaza ABI/LR/destino alterados."""
import ast,re,struct,sys
from pathlib import Path
import capstone
sys.path.insert(0,str(Path('../../verify').resolve()))
import nsoimg
product=Path(sys.argv[1]);nso=Path(sys.argv[2]);original=nsoimg.load(nso)
baseline=(product/'main.patched.bin').read_bytes();h=nsoimg.header(str(product),'main')
KEY={k[4:-4]:v for k,v in h.items() if k.startswith('KEY_') and k.endswith('_IDX')}
gates={int(a,16):(kind,int(n),int(site,16)) for a,kind,n,site in (line.split() for line in (product/'main.gates.tsv').read_text().splitlines())}
registry={}
for line in Path('../hle.h').read_text().splitlines():
    match=re.match(r'//\s+(0x[0-9a-fA-F]+)\s.*?\s(0x[0-9a-fA-F]+)（([^）]*)',line)
    if match and match[3].startswith('observación BL:'):registry[int(match[1],16)]=(int(match[2],16),match[3])
assert set(registry)=={0x99,0x9a,0x9b}
cuts=[(site,original[site:site+4],baseline[site:site+4],'observer','host') for site,_ in registry.values()]
tree=ast.parse(Path('../../verify/structck.py').read_text())
definitions=[node for node in tree.body if isinstance(node,ast.FunctionDef)]
blocks=[node for node in tree.body if isinstance(node,ast.For) and any(isinstance(n,ast.Constant) and n.value=='observación BL:' for n in ast.walk(node))]
assert len(blocks)==1
base={'re':re,'struct':struct,'capstone':capstone,'Cs':capstone.Cs,'CS_ARCH_ARM64':capstone.CS_ARCH_ARM64,'CS_MODE_ARM':capstone.CS_MODE_ARM,'orig':original,'h':h,'KEY':KEY,'PO':h['PATCH_OFF'],'_hreg':registry,'cuts':cuts,'gates':gates,'LDR_X29_SP':0xf94003fd,'W':lambda b,o:struct.unpack_from('<I',b,o)[0]}
def verify(image,gate_table=None):
    ns=base.copy();ns.update(pat=image,seen=set(),fails=[],nobserve_call=0)
    if gate_table is not None:ns['gates']=gate_table
    ns['_md']=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM);ns['_md'].detail=True
    exec(compile(ast.Module(body=definitions,type_ignores=[]),'structck_helpers','exec'),ns)
    exec(compile(ast.Module(body=blocks,type_ignores=[]),'structck_observe','exec'),ns)
    return ns['nobserve_call'],ns['fails']
assert verify(baseline)==(3,[])
print('PASS baseline:3contratos exactos')
ret=next(a for a,row in gates.items() if row[1]==0x99)
for name,offset in [('stack',-32),('scratchADRP',-24),('scratchADD',-16),('LR_ADRP',4),('LR_ADD',8),('calleeB',12)]:
    mutated=bytearray(baseline);word=struct.unpack_from('<I',mutated,ret+offset)[0];struct.pack_into('<I',mutated,ret+offset,word^1)
    valid,errors=verify(mutated);assert valid<3 and errors,(name,valid,errors)
    print('PASS rechazo',name)
mutated_gates=gates.copy();kind,n,site=mutated_gates[ret];mutated_gates[ret]=(kind,n+1,site)
assert verify(baseline,mutated_gates)[1]
print('PASS rechazo gate ID; total8casos. Usa bloque real de structck sobre imagen51, sin mutar productos.')
