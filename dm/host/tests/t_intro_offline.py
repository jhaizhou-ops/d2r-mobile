"""Comprueba el perfil privado mac-es1 sin abrir el juego ni tocar sus datos.

Uso: t_intro_offline.py <main NSO original> <directorio out> <snapshot r44 JSON>
Requiere capstone, lz4 y unicorn del entorno de generación del proyecto.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys

import lz4.block
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_W8, UC_ARM64_REG_W9, UC_ARM64_REG_W22

PROFILE = '3.3.94012-r44-mac-es1'
BUILD_ID = 'db0507f36b84200ac05f75d76ed5026f'
STORES = {
    0x801bd8: ('16dd0ab9', '1fdd0ab9'),
    0x801a20: ('28dd0ab9', '3fdd0ab9'),
    0x8019c4: ('09dd0ab9', '1fdd0ab9'),
}


def image(path):
    raw = path.read_bytes()
    assert raw[:4] == b'NSO0' and raw[0x40:0x50].hex() == BUILD_ID
    flags = struct.unpack_from('<I', raw, 12)[0]
    segments = [struct.unpack_from('<III', raw, 16 + i * 16) for i in range(3)]
    sizes = struct.unpack_from('<III', raw, 0x60)
    result = bytearray(segments[-1][1] + segments[-1][2])
    for i, (file_offset, memory_offset, size) in enumerate(segments):
        data = raw[file_offset:file_offset + sizes[i]]
        if flags >> i & 1:
            data = lz4.block.decompress(data, uncompressed_size=size)
        result[memory_offset:memory_offset + size] = data
    return result


def cuts(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith('#'):
            continue
        offset, old, new, scope, why = line.split('\t', 4)
        result[int(offset, 16)] = (old, new, scope, why)
    return result


def main():
    assert len(sys.argv) == 4, __doc__
    original = image(Path(sys.argv[1]))
    out = Path(sys.argv[2])
    snapshot = json.loads(Path(sys.argv[3]).read_text())
    active = (Path(__file__).resolve().parents[2] / 'PRODUCT').read_text().strip()
    assert active.startswith('3.3.94012-r44-mac-es')
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for kind in ('keypage', 'rules', 'touch'):
        folder = out / f'{PROFILE}-{kind}'
        candidate = (folder / 'main.patched.bin').read_bytes()
        previous = out / f'3.3.94012-r44-{kind}'
        old_image = (previous / 'main.patched.bin').read_bytes()
        assert len(old_image) == len(candidate)
        differences = {i for i, (old, new) in enumerate(zip(old_image, candidate)) if old != new}
        assert differences == {0x8019c4, 0x8019ec, 0x8019ed, 0x8019ee, 0x801a20, 0x801bd8}
        for module in ('sdk', 'subsdk0'):
            assert (folder / f'{module}.patched.bin').read_bytes() == (previous / f'{module}.patched.bin').read_bytes()
        for name in ('main.gates.tsv', 'main.sites.tsv', 'main.obs.tsv', 'main.fixups.tsv', 'main.params.tsv'):
            assert (folder / name).read_bytes() == (previous / name).read_bytes()
        if kind != 'keypage':
            rules = (folder / 'main.rules.tsv').read_text().splitlines()
            before = (previous / 'main.rules.tsv').read_text().splitlines()
            assert rules[:3] == before[:3] and rules[4:] == before[4:]
            assert rules[3] == '# IMAGE_SHA256\t' + hashlib.sha256(candidate).hexdigest()
        assert f'# BUILD_ID\t{BUILD_ID}' in (folder / 'main.cuts.tsv').read_text()
        assert original[0x8019ec:0x8019f0] == candidate[0x8019ec:0x8019f0] == bytes.fromhex('280f0034')
        assert candidate[0x8019e0:0x8019e4] == original[0x8019e0:0x8019e4]
        registered = cuts(folder / 'main.cuts.tsv')
        baseline = cuts(out / f'3.3.94012-r44-{kind}' / 'main.cuts.tsv')
        assert set(registered) == (set(baseline) - {0x8019ec}) | set(STORES)
        for offset, row in baseline.items():
            if offset != 0x8019ec:
                assert registered[offset] == row, (kind, hex(offset))
        for offset, (old, new) in STORES.items():
            assert original[offset:offset + 4] == bytes.fromhex(old)
            assert candidate[offset:offset + 4] == bytes.fromhex(new)
            assert registered[offset][:2] == (old, new)
            assert registered[offset][2] == '全'
            instruction = next(disassembler.disasm(candidate[offset:offset + 4], offset))
            assert instruction.mnemonic == 'str' and instruction.op_str.startswith('wzr,')
        print(f'{kind}: seis bytes distintos; rama original y tres stores offline; SDK, puertas, reglas y resto de CUTS conservados.')
    for start, end, register in (
        (0x801bd0, 0x801bdc, UC_ARM64_REG_W22),
        (0x801a18, 0x801a24, UC_ARM64_REG_W8),
        (0x8019bc, 0x8019c8, UC_ARM64_REG_W9),
    ):
        for modified in (False, True):
            machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            machine.mem_map(0x801000, 0x1000)
            machine.mem_map(0x2d7b000, 0x1000)
            machine.mem_write(0x801000, bytes(original[0x801000:0x802000]))
            machine.reg_write(register, 1)
            if modified:
                for offset, (_, new) in STORES.items():
                    machine.mem_write(offset, bytes.fromhex(new))
            machine.emu_start(start, end)
            mode = struct.unpack('<I', machine.mem_read(0x2d7badc, 4))[0]
            assert mode == (0 if modified else 1)
            assert machine.reg_read(register) == 1
        print(f'Unicorn {start:#x}: modo 1 → 0 y registro fuente conservado.')
    for name, digest in snapshot.items():
        assert hashlib.sha256(Path(name).read_bytes()).hexdigest() == digest, name
    print(f'Perfil privado verificado. {len(snapshot)} archivos r44 anteriores conservan su SHA-256.')


if __name__ == '__main__':
    main()
