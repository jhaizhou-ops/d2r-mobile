# 用闪退现场的索引执行改写后的 effect.age_pCtxI，核对逐项和常量输入。
# Run the rewritten effect.age_pCtxI with the crash indices, checking both per-lane and constant inputs.
# python3 dm/verify/test_effect_age_lookup.py <main.patched.bin>
import struct
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import (
    UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP,
    UC_ARM64_REG_X0, UC_ARM64_REG_X1,
)

from nsoimg import header


ENTRY = 0x144F2C0
RETURN = 0x144F650
CRASH_IDS = (0x3EB2580F, 0x3C81DBF6, 0x3FA28000)


def lookup(image, patch_offset, ids, expected, *, constant=False, count=3, has_table=True):
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    code_page = ENTRY & ~0xFFF
    machine.mem_map(code_page, 0x1000)
    machine.mem_write(code_page, image[code_page:code_page + 0x1000])
    patch = image[patch_offset:]
    machine.mem_map(patch_offset, (len(patch) + 0xFFF) & ~0xFFF)
    machine.mem_write(patch_offset, patch)

    base = 0x10000000
    machine.mem_map(base, 0x10000)
    output, args, source, emitter = (base + offset for offset in (0x100, 0x200, 0x300, 0x400))
    context, indices, result = (base + offset for offset in (0x600, 0x800, 0x900))
    deltas, factors = base + 0xA00, base + 0xB00
    table = 0x20000FF0
    machine.mem_map(table & ~0xFFF, 0x1000)

    def write(address, fmt, *values):
        machine.mem_write(address, struct.pack('<' + fmt, *values))

    write(output, 'QII', result, len(expected), 4)
    write(args + 8, 'Q', source)
    write(source, 'Q', emitter)
    write(source + 0x10, 'QQ', context, indices)
    write(source + 0x28, 'I', ids[0] if constant else 0xFFFFFFFF)
    write(context + 0x18, 'QII', table if has_table else 0, count, 4)
    write(table, '4f', 0.5, 2.5, 4.0, 42.0)
    write(indices, f'{len(ids)}I', *ids)
    write(emitter + 0xC8, 'Q', factors)
    write(emitter + 0xD8, 'Q', deltas)
    write(emitter + 0xE4, 'I', 0 if constant else 4)
    write(factors, 'f', 2.0)
    delta = [0.5, -1.0, 1.0, -2.0, 1.0, 0.0, -1.0, 1.0]
    write(deltas, f'{len(delta)}f', *delta)

    sentinel = b'\xa5' * 4
    machine.mem_write(result - 4, sentinel * (len(expected) + 2))
    for reg, value in ((UC_ARM64_REG_X0, output), (UC_ARM64_REG_X1, args),
                       (UC_ARM64_REG_SP, base + 0xF000), (UC_ARM64_REG_LR, RETURN)):
        machine.reg_write(reg, value)
    try:
        machine.emu_start(ENTRY, RETURN, count=2000)
    except UcError as error:
        raise AssertionError(f'Effect age faulted at {machine.reg_read(UC_ARM64_REG_PC):#x} for {ids!r}') from error
    assert machine.reg_read(UC_ARM64_REG_PC) == RETURN, 'Effect age lookup did not finish'
    expected_bytes = sentinel + struct.pack(f'<{len(expected)}f', *expected) + sentinel
    actual = bytes(machine.mem_read(result - 4, len(expected_bytes)))
    assert actual == expected_bytes, (ids, actual.hex(), expected_bytes.hex())


if __name__ == '__main__':
    path = Path(sys.argv[1])
    image = path.read_bytes()
    patch_offset = header(path.parent, 'main')['PATCH_OFF']
    lookup(image, patch_offset, (2, CRASH_IDS[0], 1, CRASH_IDS[1], CRASH_IDS[2], 0, 3, 2),
           (3., 0., 0.5, 0., 0., 0.5, 0., 2.))
    lookup(image, patch_offset, (0, 1, 2), (0., 4.5, 2.))
    lookup(image, patch_offset, (0, 0xFFFFFFFF), (0., 0.), count=0)
    lookup(image, patch_offset, (2, 1), (0., 0.), has_table=False)
    lookup(image, patch_offset, (2,), (3.,) * 9, constant=True)
    lookup(image, patch_offset, (CRASH_IDS[0],), (0.,) * 9, constant=True)
    lookup(image, patch_offset, (3,), (0.,) * 3, constant=True)
    lookup(image, patch_offset, (0,), (0.,), constant=True, has_table=False)
    print('Effect age lookup OK (8 cases)')
