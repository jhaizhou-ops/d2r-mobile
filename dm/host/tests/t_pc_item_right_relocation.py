"""Gate9e relocalizado: retorno virtual real, frontera C simulada y sin GUI."""
from pathlib import Path
import ast
import hashlib
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r

site = 0x3ca58c
source = Path('dm/tools/patch_nso.py')
original = Path('dm/host/tests/build/main.orig.bin').read_bytes()
assert hashlib.sha256(original).hexdigest() == '6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
if len(sys.argv) > 1:
    product = Path(sys.argv[1])
    image = (product / 'main.patched.bin').read_bytes()
    rows = [line.split('\t') for line in (product / 'main.gates.tsv').read_text().splitlines() if line and not line.startswith('#')]
    ret = next(int(row[0], 16) for row in rows if row[1:] == ['svc', '158', hex(site)])
    headers = {line[2:].split('\t')[0]: int(line.split('\t')[1], 0) for line in (product / 'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
    key_page, key_index = headers['KEY_PAGE_OFF'], headers['KEY_KGATE_IDX']
    mode = 'producto'
else:
    tree = ast.parse(source.read_text())
    names = {'B', 'BR', 'STP_PRE', 'LDP_POST', 'MRS_TPIDRRO', 'ADRP', 'ADDI', 'LDRX', 'ADDR', 'BLR', 'emit_gate_call', 'emit_slot_addr', 'emit_key_addr'}
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    block = next(node for node in tree.body if isinstance(node, ast.If) and any(isinstance(child, ast.Constant) and child.value == 'pc_item_right' for child in ast.walk(node)))
    patch, gates, cuts = bytearray(), [], []
    patch_off, key_page, key_index = 0x3400000, 0x6000000, 0
    env = dict(struct=struct, IMG=original, RULE_RUN=True, PRIVATE_GATES=True, TEXT_END=0x1b80000, kinds={}, windows=[],
               patch=patch, gates=gates, cuts=cuts, fix=[], KEY_PAGE_OFF=key_page, KEY_INDEX={'KGATE': 0})
    env.update(here=lambda: patch_off + len(patch), emit=lambda word: patch.extend(struct.pack('<I', word)), stat=lambda *_: None)
    branches = []
    env['put_branch'] = lambda at, target: branches.append((at, target))
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), 'exec'), env)
    exec(compile(ast.Module(body=[block], type_ignores=[]), str(source), 'exec'), env)
    assert len(branches) == len(gates) == 1 and gates[0][1:] == ('svc', 158, site)
    ret = gates[0][0]
    image = bytearray(original)
    image.extend(bytes(patch_off + len(patch) - len(image)))
    image[patch_off:] = patch
    struct.pack_into('<I', image, site, env['B'](*branches[0]))
    mode = 'AST de producción, sin PRODUCT'

registers = [getattr(r, f'UC_ARM64_REG_X{i}') for i in range(31)]
failures = checks = 0
print('Gate9e', mode, 'SHA256', hashlib.sha256(image).hexdigest())
for base in (0, 0x100000000, 0x107f3d000, 0x7000000000):
    for handled in (0, 1):
        for flags in (0, 0xa0000000, 0xf0000000):
            u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            u.mem_map(base, (len(image) + 4095) & ~4095)
            u.mem_write(base, bytes(image))
            if key_page >= ((len(image) + 4095) & ~4095):
                u.mem_map(base + (key_page & ~4095), 0x10000)
            for page in (0x7000000, 0x8000000, 0x9000000):
                u.mem_map(page, 0x10000)
            u.mem_write(base + key_page + 8 * key_index, struct.pack('<Q', 0x20))
            u.mem_write(0x8000120, struct.pack('<Q', 0x9000000))
            u.mem_write(0x9000200, struct.pack('<I', 0xd65f03c0))  # RET original del doble virtual.
            u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0, 0x8000100)
            values = [0x1110000 + i * 16 for i in range(31)]
            values[0] = values[20] = 0x9003000
            values[1], values[8] = 4 | (2 << 32), 0x9000200
            for reg, value in zip(registers, values):
                u.reg_write(reg, value)
            sp = 0x7008000
            u.reg_write(r.UC_ARM64_REG_SP, sp)
            u.reg_write(r.UC_ARM64_REG_NZCV, flags)
            callbacks, native_lrs = [], []

            def hook(cpu, address, size, data):
                if address == 0x9000000:
                    callbacks.append(cpu.reg_read(r.UC_ARM64_REG_LR))
                    cpu.reg_write(registers[8], handled)
                    cpu.reg_write(r.UC_ARM64_REG_PC, base + ret)
                elif address == 0x9000200:
                    native_lrs.append(cpu.reg_read(r.UC_ARM64_REG_LR))

            u.hook_add(UC_HOOK_CODE, hook)
            error = None
            try:
                u.emu_start(base + site, base + site + 4, count=200)
            except UcError as exc:
                error = str(exc)
            expected = values[:]
            if not handled:
                expected[30] = base + site + 4
            valid = (not error and callbacks == [base + ret] and native_lrs == ([] if handled else [base + site + 4])
                     and [u.reg_read(reg) for reg in registers] == expected
                     and u.reg_read(r.UC_ARM64_REG_PC) == base + site + 4
                     and u.reg_read(r.UC_ARM64_REG_SP) == sp and u.reg_read(r.UC_ARM64_REG_NZCV) == flags)
            checks += 1
            failures += not valid
            print('PASS' if valid else 'FAIL', 'base', hex(base), 'handled', handled, 'NZCV', hex(flags),
                  'nativeLR', [hex(value) for value in native_lrs], 'pc', hex(u.reg_read(r.UC_ARM64_REG_PC)), error or '')
print(f'Gate9e ASLR {checks} comprobaciones, {failures} fallos; cuatro bases, RET virtual ejecutado, C simulado; sin GUI.')
raise SystemExit(bool(failures))
