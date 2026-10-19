"""Puerta9c real: rama PausePanel/Close y epílogo; host simulado."""
from pathlib import Path
import hashlib
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as r

p = Path(sys.argv[1])
image = (p / 'main.patched.bin').read_bytes()
rows = [line.split('\t') for line in (p / 'main.gates.tsv').read_text().splitlines() if line and not line.startswith('#')]
ret = next(int(row[0], 16) for row in rows if row[1:] == ['svc', '156', '0xae5324'])
headers = {line[2:].split('\t')[0]: int(line.split('\t')[1], 0) for line in (p / 'main.fixups.tsv').read_text().splitlines() if line.startswith('# ')}
regs = [getattr(r, f'UC_ARM64_REG_X{i}') for i in range(31)]
print('SHA256', hashlib.sha256(image).hexdigest())
for pc in (0, 1):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0, (len(image) + 4095) & ~4095)
    u.mem_write(0, image)
    for page in (0x7000000, 0x8000000, 0x9000000):
        u.mem_map(page, 0x10000)
    u.mem_write(headers['KEY_PAGE_OFF'] + headers['KEY_KGATE_IDX'] * 8, struct.pack('<Q', 0x20))
    u.mem_write(0x8000120, struct.pack('<Q', 0x9000000))
    u.reg_write(r.UC_ARM64_REG_TPIDRRO_EL0, 0x8000100)
    sp = 0x7008000
    u.reg_write(r.UC_ARM64_REG_SP, sp)
    u.reg_write(r.UC_ARM64_REG_NZCV, 0xa0000000)
    values = [0x1110000 + i * 16 for i in range(31)]
    for reg, value in zip(regs, values):
        u.reg_write(reg, value)
    calls = []
    def host(cpu, address, size, opaque):
        if address == 0x9000000:
            calls.append(address)
            assert cpu.reg_read(r.UC_ARM64_REG_LR) == ret
            cpu.reg_write(regs[8], pc)
            cpu.reg_write(r.UC_ARM64_REG_PC, ret)
    u.hook_add(UC_HOOK_CODE, host)
    target = 0xae53d0 if pc else 0xae5328
    u.emu_start(0xae5324, target, count=100)
    expected = values[:]
    if not pc:
        expected[8] = 0x452c
    assert calls == [0x9000000] and [u.reg_read(reg) for reg in regs] == expected
    assert u.reg_read(r.UC_ARM64_REG_SP) == sp and u.reg_read(r.UC_ARM64_REG_NZCV) == 0xa0000000
    if pc:
        saved = [0x2220, 0x2219, 0x2229, 0x9000100]
        u.mem_write(sp + 0x20, struct.pack('<4Q', *saved))
        u.emu_start(target, saved[3], count=10)
        assert u.reg_read(r.UC_ARM64_REG_SP) == sp + 0x40
        assert [u.reg_read(regs[i]) for i in (20, 19, 29, 30)] == saved
    print(f'PASS9c PC={pc}: destino {target:x}, MOVZ sólo legacy; registros/SP/NZCV y epílogo PC conservados')
print('LÍMITE: cuerpo host simulado; handler Close y ciclo OnClose originales se ensayan aparte; sin GUI.')
