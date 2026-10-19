"""Unicorn: observadores99–9b transparentes, con destino y LR del BL original."""
import ast
import hashlib
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r

sites=((0x2b2ebc,0x99,0x4ace20),(0x2b2f38,0x9a,0x4ace20),(0x2b3124,0x9b,0x4b83b0))
if len(sys.argv)>1:
    product=Path(sys.argv[1]);image=(product/'main.patched.bin').read_bytes()
    rows=[line.split('\t') for line in (product/'main.gates.tsv').read_text().splitlines() if line and not line.startswith('#')]
    headers={line[2:].split('\t')[0]:int(line.split('\t')[1],0) for line in (product/'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
    keypage,keyindex=headers['KEY_PAGE_OFF'],headers['KEY_KGATE_IDX']
else:
    # Ejecuta el emisor real del patcher en una imagen aislada, sin construir productos.
    original=Path('build/main.orig.bin').read_bytes()
    assert hashlib.sha256(original).hexdigest()=='6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
    tree=ast.parse(Path('../../tools/patch_nso.py').read_text())
    definitions=[node for node in tree.body if isinstance(node,ast.FunctionDef)]
    blocks=[node for node in tree.body if isinstance(node,ast.If) and any(isinstance(n,ast.Constant) and n.value==0x9407e7d9 for n in ast.walk(node))]
    assert len(blocks)==1
    keypage,keyindex=0x3200000,3
    ns={'struct':struct,'IMG':bytearray(original),'PATCH_OFF':0x3300000,'KEY_PAGE_OFF':keypage,'KEY_INDEX':{'KGATE':keyindex},'patch':bytearray(),'fix':[],'gates':[],'cuts':[],'stats':{},'RULE_RUN':True,'PRIVATE_GATES':True,'TEXT_END':0x1b78000,'kinds':{},'windows':[]}
    exec(compile(ast.Module(body=definitions,type_ignores=[]),'patch_nso_helpers','exec'),ns)
    exec(compile(ast.Module(body=blocks,type_ignores=[]),'patch_nso_observers','exec'),ns)
    image=ns['IMG'];image.extend(bytes(0x3300000-len(image)));image.extend(ns['patch']);image=bytes(image)
    rows=[[hex(a),kind,str(number),hex(site)] for a,kind,number,site in ns['gates']]
print('IMAGE SHA256',hashlib.sha256(image).hexdigest())
registers=[getattr(r,f'UC_ARM64_REG_X{i}') for i in range(31)]
passed=0
for desktop in (0,1):
    for site,number,callee in sites:
        matching=[int(row[0],16) for row in rows if row[1:]==['svc',str(number),hex(site)]]
        assert len(matching)==1,(hex(site),matching)
        ret=matching[0]
        u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0,(len(image)+4095)&~4095);u.mem_write(0,image)
        for page in (0x7000000,0x8000000,0x9000000):u.mem_map(page,0x10000)
        u.mem_write(keypage+keyindex*8,struct.pack('<Q',0x20));u.mem_write(0x8000120,struct.pack('<Q',0x9000000))
        u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0,0x8000100);u.reg_write(r.UC_ARM64_REG_SP,0x7008000);u.reg_write(r.UC_ARM64_REG_NZCV,0xa0000000)
        values=[0x1110000+i*16 for i in range(31)]
        for reg,value in zip(registers,values):u.reg_write(reg,value)
        calls=[]
        def hook(cpu,address,size,opaque):
            if address==0x9000000:
                assert cpu.reg_read(r.UC_ARM64_REG_LR)==ret
                assert [cpu.reg_read(reg) for reg in registers[:29]]==values[:29]
                calls.append(('observe',number,desktop));cpu.reg_write(r.UC_ARM64_REG_PC,ret)
            elif address==callee:
                expected=values[:];expected[30]=site+4
                assert [cpu.reg_read(reg) for reg in registers]==expected
                assert cpu.reg_read(r.UC_ARM64_REG_SP)==0x7008000 and cpu.reg_read(r.UC_ARM64_REG_NZCV)==0xa0000000
                calls.append(('native',callee));cpu.reg_write(registers[0],0xbeef);cpu.reg_write(r.UC_ARM64_REG_PC,site+4)
        u.hook_add(UC_HOOK_CODE,hook);u.emu_start(site,site+4,count=100)
        assert calls==[('observe',number,desktop),('native',callee)]
        assert u.reg_read(registers[0])==0xbeef and u.reg_read(r.UC_ARM64_REG_LR)==site+4
        print('PASS',hex(number),'PC',desktop,'BL una vez, GPR/SP/NZCV intactos, LR original',hex(site+4));passed+=1
print('PASS',passed,'casos ABI; observador host y callee son sinks explícitos; no prueba de mundo vivo.')
