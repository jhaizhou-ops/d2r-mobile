"""Puertas93–95 del producto real; la frontera host está simulada expresamente."""
from pathlib import Path
import hashlib
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r

product = Path(sys.argv[1])
image = (product / 'main.patched.bin').read_bytes()
rows = [s.split('\t') for s in (product / 'main.gates.tsv').read_text().splitlines() if s and not s.startswith('#')]
headers = {line[2:].split('\t')[0]: int(line.split('\t')[1], 0) for line in (product / 'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
regs = [getattr(r, f'UC_ARM64_REG_X{i}') for i in range(31)]
print('SHA256', hashlib.sha256(image).hexdigest())

def machine():
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0, (len(image) + 4095) & ~4095)
    u.mem_write(0, image)
    for page in (0x7000000, 0x8000000, 0x9000000):
        u.mem_map(page, 0x10000)
    u.mem_write(headers['KEY_PAGE_OFF'] + headers['KEY_KGATE_IDX'] * 8, struct.pack('<Q', 0x20))
    u.mem_write(0x8000120, struct.pack('<Q', 0x9000000))
    u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0, 0x8000100)
    u.reg_write(r.UC_ARM64_REG_SP, 0x7008000)
    u.reg_write(r.UC_ARM64_REG_NZCV, 0xa0000000)
    return u

cases = 0
for pc in (0, 1):
    for site, svc, target in ((0xad8a54, 0x93, 0xad8a58), (0x456c5c, 0x94, 0x456d50 if pc else 0x456c60), (0xaecca4, 0x95, 0xaecca8)):
        u = machine()
        values = [0x1110000 + i * 16 for i in range(31)]
        for reg, value in zip(regs, values):
            u.reg_write(reg, value)
        ret = next(int(row[0], 16) for row in rows if row[1:] == ['svc', str(svc), hex(site)])
        calls = []
        def host(cpu, address, size, opaque):
            if address == 0x9000000:
                calls.append(address)
                assert cpu.reg_read(r.UC_ARM64_REG_LR) == ret
                if svc == 0x95:
                    assert [cpu.reg_read(reg) for reg in regs[:3]] == values[:3]
                    cpu.reg_write(regs[0], 0xbeef)
                else:
                    cpu.reg_write(regs[8], pc if svc == 0x94 else 0)
                cpu.reg_write(r.UC_ARM64_REG_PC, ret)
        u.hook_add(UC_HOOK_CODE, host)
        u.emu_start(site, target, count=100)
        expected = values[:]
        sp = 0x7008000
        if svc == 0x93:
            sp -= 0xe0
        if svc == 0x94 and not pc:
            expected[8] = sp + 0x6fd
        if svc == 0x95:
            expected[0] = 0xbeef
        assert calls == [0x9000000]
        assert [u.reg_read(reg) for reg in regs] == expected
        assert u.reg_read(r.UC_ARM64_REG_SP) == sp
        assert u.reg_read(r.UC_ARM64_REG_NZCV) == 0xa0000000
        cases += 1
        print(f'PASS ABI {svc:x} PC={pc}: destino {target:x}, registros/SP/NZCV')

tab, stop = 0x8004000, 0x9000100
for pc in (0, 1):
    for switching in (0, 1):
        for indicators in (0, 1):
            for phase in ('init', 'update'):
                u = machine()
                u.mem_write(tab + 0xda0, bytes([switching, indicators]))
                u.reg_write(regs[0], tab)
                u.reg_write(r.UC_ARM64_REG_LR, stop)
                reached = []
                def body(cpu, address, size, opaque):
                    if address == 0x9000000:
                        if pc:
                            cpu.mem_write(tab + 0xda1, b'\0')
                        cpu.reg_write(regs[8], 0)
                        cpu.reg_write(r.UC_ARM64_REG_PC, cpu.reg_read(r.UC_ARM64_REG_LR))
                    elif phase == 'init' and address == 0xad8a74:
                        cpu.reg_write(regs[19], tab)
                        cpu.reg_write(r.UC_ARM64_REG_PC, 0xad8f38)
                    elif phase == 'init' and address == 0xad9240:
                        cpu.reg_write(r.UC_ARM64_REG_PC, cpu.reg_read(r.UC_ARM64_REG_LR))
                    elif address == (0xad8f50 if phase == 'init' else 0xad97d0):
                        reached.append(address)
                        cpu.emu_stop()
                if phase == 'update' and pc:
                    u.mem_write(tab + 0xda1, b'\0')
                u.hook_add(UC_HOOK_CODE, body)
                u.emu_start(0xad8a54 if phase == 'init' else 0xad97ac, stop, count=1000)
                assert bool(reached) == bool(not pc and switching and indicators)
                assert u.mem_read(tab + 0xda0, 1) == bytes([switching])
                assert u.mem_read(tab + 0xda1, 1) == bytes([0 if pc else indicators])
                cases += 1
def cstring(u, address):
    out = bytearray()
    while (value := u.mem_read(address + len(out), 1)) != b'\0':
        out.extend(value)
    return bytes(out)

for pc in (0, 1):
    for indicator in (0, 1):
        u = machine()
        sp, name = 0x7008000, 'Cadáver de TestPlayer'.encode()
        u.mem_write(sp + 0x58, name + b'\0')
        u.mem_write(sp + 0x4000, '\ue013'.encode() + b'\0')
        values = {0: sp + 0x58, 19: 0x20b8180, 20: 0x200000001, 21: 0x6400000064, 22: 0x1cf89e9, 23: 8, 25: indicator, 26: 1, 27: sp + 0xf58}
        for index, value in values.items():
            u.reg_write(regs[index], value)
        names, indicators = [], []
        def labels(cpu, address, size, opaque):
            if address == 0x9000000:
                cpu.reg_write(regs[8], pc)
                cpu.reg_write(r.UC_ARM64_REG_PC, cpu.reg_read(r.UC_ARM64_REG_LR))
            elif address in (0xb16660, 0xb167b0):
                (names if address == 0xb16660 else indicators).append(cstring(cpu, cpu.reg_read(regs[0])))
                cpu.reg_write(r.UC_ARM64_REG_PC, cpu.reg_read(r.UC_ARM64_REG_LR))
            elif address == 0x1b76040:
                dst, src, count = [cpu.reg_read(reg) for reg in regs[:3]]
                cpu.mem_write(dst, bytes(cpu.mem_read(src, count)))
                cpu.reg_write(r.UC_ARM64_REG_PC, cpu.reg_read(r.UC_ARM64_REG_LR))
            elif address == 0x1b76070:
                cpu.reg_write(regs[0], len(cstring(cpu, cpu.reg_read(regs[0]))))
                cpu.reg_write(r.UC_ARM64_REG_PC, cpu.reg_read(r.UC_ARM64_REG_LR))
            elif address == 0x7f5630:
                descriptor = cpu.reg_read(regs[0])
                key = struct.unpack('<Q', cpu.mem_read(descriptor, 8))[0]
                assert cstring(cpu, key) == b'ControllerInteractButton'
                cpu.reg_write(regs[0], sp + 0x4000)
                cpu.reg_write(r.UC_ARM64_REG_PC, cpu.reg_read(r.UC_ARM64_REG_LR))
        u.hook_add(UC_HOOK_CODE, labels)
        u.emu_start(0x456c54, 0x456d50, count=500)
        assert names == [name] and len(indicators) == bool(indicator and not pc)
        assert u.reg_read(regs[25]) == indicator
        cases += 1
        print(f'PASS nombre cadáver producto PC={pc}, indicador={indicator}: nombre intacto, etiqueta mando={len(indicators)}')
print(f'PASS {cases} casos: ABI93/94/95, init/update TabBar y nombre de cadáver')
print('LÍMITE: handlers host y construcción previa de pestañas simulados; sin GUI ni receptor gráfico de nombres.')
