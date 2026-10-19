"""Verifica productos mac-es2 y la ABI del trampolín OBS de glifos.

Uso: t_glyph_obs.py <directorio out> <snapshot es2-previo.json>
"""
import hashlib
import json
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as registers


def rows(path):
    return [line.split('\t') for line in path.read_text().splitlines() if not line.startswith('#')]


def branch(image, address):
    word = struct.unpack_from('<I', image, address)[0]
    assert word & 0xfc000000 == 0x14000000
    displacement = word & 0x3ffffff
    if displacement & 0x2000000:
        displacement -= 0x4000000
    return address + displacement * 4


def main():
    assert len(sys.argv) == 3, __doc__
    root = Path(sys.argv[1])
    for kind in ('keypage', 'rules', 'touch'):
        previous = root / f'3.3.94012-r44-mac-es1-{kind}'
        current = root / f'3.3.94012-r44-mac-es2-{kind}'
        for module in ('sdk', 'subsdk0'):
            assert (previous / f'{module}.patched.bin').read_bytes() == (current / f'{module}.patched.bin').read_bytes()
        old_obs = rows(previous / 'main.obs.tsv')
        new_obs = rows(current / 'main.obs.tsv')
        image = (current / 'main.patched.bin').read_bytes()
        for address, value in {0x8019ec:'280f0034', 0x801bd8:'1fdd0ab9', 0x801a20:'3fdd0ab9', 0x8019c4:'1fdd0ab9'}.items():
            assert image[address:address + 4] == bytes.fromhex(value)
        if kind != 'touch':
            assert (previous / 'main.patched.bin').read_bytes() == image
            assert old_obs == new_obs
            assert image[0xad75f0:0xad75f4] == bytes.fromhex('ff8305d1')
            print(f'{kind}: idéntico a mac-es1, sin adaptación de glifos.')
            continue
        assert new_obs[:-1] == old_obs
        assert new_obs[-1][:3] == ['0xad75f0', '23', 'd10583ff']
        old_gates = {(r[1], r[2], r[3]) for r in rows(previous / 'main.gates.tsv')}
        new_gates = {(r[1], r[2], r[3]) for r in rows(current / 'main.gates.tsv')}
        assert new_gates == old_gates | {('obs', '23', '0xad75f0')}
        headers = {}
        for line in (current / 'main.fixups.tsv').read_text().splitlines():
            if line.startswith('# '):
                key, value = line[2:].split('\t')
                headers[key] = int(value, 0)
        start = branch(image, 0xad75f0)
        ret = next(int(row[0], 16) for row in rows(current / 'main.gates.tsv') if row[1:] == ['obs', '23', '0xad75f0'])
        assert headers['PATCH_OFF'] <= start < ret < len(image)
        assert image[ret + 4:ret + 8] == bytes.fromhex('ff8305d1')
        assert branch(image, ret + 8) == 0xad75f4
        disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        print('touch: OBS23 exclusivo, prólogo y salida originales comprobados.')
        for instruction in disassembler.disasm(image[start:ret + 12], start):
            print(f'  {instruction.address:#x} {instruction.mnemonic} {instruction.op_str}')
        for replace_path in (False, True):
            machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            machine.mem_map(0, (len(image) + 4095) & ~4095)
            machine.mem_write(0, image)
            machine.mem_map(0x7000000, 0x10000)
            machine.mem_map(0x8000000, 0x1000)
            machine.mem_map(0x9000000, 0x1000)
            machine.mem_write(headers['KEY_PAGE_OFF'] + headers['KEY_KGATE_IDX'] * 8, struct.pack('<Q', 0x20))
            machine.mem_write(0x8000120, struct.pack('<Q', 0x9000000))
            machine.reg_write(registers.UC_ARM64_REG_TPIDRRO_EL0, 0x8000100)
            machine.reg_write(registers.UC_ARM64_REG_SP, 0x7008000)
            machine.reg_write(registers.UC_ARM64_REG_NZCV, 0xa0000000)
            values = {i:0x1110000 + i * 16 for i in range(31)}
            for index, value in values.items():
                machine.reg_write(getattr(registers, f'UC_ARM64_REG_X{index}'), value)
            calls = []

            def host_gate(cpu, address, size, opaque):
                if address != 0x9000000:
                    return
                calls.append(address)
                assert cpu.reg_read(registers.UC_ARM64_REG_LR) == ret
                for index in (0, 1, 2, 8, 29):
                    assert cpu.reg_read(getattr(registers, f'UC_ARM64_REG_X{index}')) == values[index]
                if replace_path:
                    cpu.reg_write(registers.UC_ARM64_REG_X1, 0xbeef100)
                cpu.reg_write(registers.UC_ARM64_REG_PC, ret)

            machine.hook_add(UC_HOOK_CODE, host_gate)
            machine.emu_start(0xad75f0, 0xad75f4, count=100)
            assert len(calls) == 1
            assert machine.reg_read(registers.UC_ARM64_REG_PC) == 0xad75f4
            assert machine.reg_read(registers.UC_ARM64_REG_SP) == 0x7008000 - 0x160
            assert machine.reg_read(registers.UC_ARM64_REG_NZCV) == 0xa0000000
            for index, value in values.items():
                expected = 0xbeef100 if replace_path and index == 1 else value
                assert machine.reg_read(getattr(registers, f'UC_ARM64_REG_X{index}')) == expected
            print(f'Unicorn: ruta {"sustituida" if replace_path else "conservada"}, resto de registros y flags intactos; SP original ejecutado una vez.')
    snapshot = json.loads(Path(sys.argv[2]).read_text())
    for path, expected in snapshot.items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected
    print(f'Integridad: {len(snapshot)} archivos r44 y mac-es1 anteriores conservados.')


if __name__ == '__main__':
    main()
