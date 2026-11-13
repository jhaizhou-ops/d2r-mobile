# 在 Unicorn 中运行改写后的 3.3.94012 资源查找，复现 iPad 换图时遇到的空页。
# Run the rewritten 3.3.94012 resource lookup in Unicorn with the missing page seen during an iPad area change.
# python3 dm/verify/test_area_resource_lookup.py <main.patched.bin>
import struct
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import (
    UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP,
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X3, UC_ARM64_REG_X4,
)

ENTRY = 0x1450040
RETURN = 0x14502E0
MISSING_PAGE_ID = 0xB20
VALID_VECTORS = {0x204: (1., -2., 3.), 0x407: (4., 5., -6.)}
ZERO = (0., 0., 0.)


def lookup(code, ids, expected, page_count=6, has_table=True):
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    code_page = ENTRY & ~0xFFF
    machine.mem_map(code_page, 0x1000)
    machine.mem_write(ENTRY, code)

    base = 0x10000000
    machine.mem_map(base, 0x10000)
    output, header, args, source = (base + offset for offset in (0x100, 0x200, 0x400, 0x500))
    context, indices, result = (base + offset for offset in (0x700, 0x900, 0xA00))
    page_one, page_two = base + 0x1000, base + 0x2000
    table_page = 0x20000000
    machine.mem_map(table_page, 0x1000)
    table = table_page + 0x1000 - 6 * 8

    def write(address, fmt, *values):
        machine.mem_write(address, struct.pack('<' + fmt, *values))

    write(output, 'QII', result, len(ids), 16)
    write(args, 'QQ', header, source)
    write(header + 0xC8, 'Q', base)
    write(source + 8, 'Q', context)
    write(source + 0x18, 'Q', indices)
    write(source + 0x28, 'I', 0xFFFFFFFF)
    write(indices, f'{len(ids)}I', *ids)
    write(context + 0x1A0, 'Q', table if has_table else 0)
    write(context + 0x1B0, 'I', page_count)
    write(table + 8, 'QQ', page_one, page_two)
    write(page_one + 2 * 8, 'Q', base + 0x3000)  # 向量地址为空 | null vector pointer

    for index, (entry_id, vector) in enumerate(VALID_VECTORS.items()):
        page = (page_one, page_two)[index]
        entry = base + 0x4000 + index * 0x100
        vectors = base + 0x5000 + index * 0x100
        write(page + (entry_id & 0x1FF) * 8, 'Q', entry)
        write(entry + 0x10 + 0x48, 'Q', vectors)
        write(vectors + 3 * 16, '4f', *vector, 42.)

    sentinel = b'\xa5' * 4
    machine.mem_write(result, sentinel * (4 * len(ids)))
    for reg, value in ((UC_ARM64_REG_X0, output), (UC_ARM64_REG_X1, args),
                       (UC_ARM64_REG_X3, 0x10), (UC_ARM64_REG_X4, 3),
                       (UC_ARM64_REG_SP, base + 0xF000), (UC_ARM64_REG_LR, RETURN)):
        machine.reg_write(reg, value)
    try:
        machine.emu_start(ENTRY, RETURN, count=1000)
    except UcError as error:
        raise AssertionError(f'Lookup faulted at {machine.reg_read(UC_ARM64_REG_PC):#x} for {ids!r}') from error
    assert machine.reg_read(UC_ARM64_REG_PC) == RETURN, 'Lookup did not finish'
    expected_bytes = b''.join(struct.pack('<3f', *value) + sentinel for value in expected)
    assert bytes(machine.mem_read(result, len(expected_bytes))) == expected_bytes, (ids, expected)


if __name__ == '__main__':
    code = Path(sys.argv[1]).read_bytes()[ENTRY:RETURN]
    first, second = VALID_VECTORS.values()
    lookup(code, (0xFFFFFFFF, 0x204, 0x204, 0x407), (ZERO, first, first, second))
    lookup(code, (0x204, MISSING_PAGE_ID, MISSING_PAGE_ID, 0x407), (first, ZERO, ZERO, second))
    lookup(code, (0x407, 0x21F, 0x202, 0x204, 0xFFFFFFFF, 0xE00), (second, ZERO, ZERO, first, ZERO, ZERO))
    lookup(code, (MISSING_PAGE_ID, 0x204), (ZERO, first), page_count=5)
    lookup(code, (0x204,), (ZERO,), page_count=1)
    lookup(code, (0x204,), (ZERO,), has_table=False)
    print('Area resource lookup OK (6 cases)')
