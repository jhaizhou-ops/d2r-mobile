# PC 物品门的独立逐字验证；依据 3.3.94012 的七个调用点和 AAPCS64 保存契约。
# Independent word-for-word PC item-gate verification, based on seven 3.3.94012 sites and the AAPCS64 save contract.
import struct

SITES = {0x9d: (0x3ca47c, 0x54000920), 0x9e: (0x3ca58c, 0xd63f0100),
         0x9f: (0x3b8648, None), 0xa0: (0x3db0b4, 0x37001000),
         0xa1: (0xabd428, None), 0xa2: (0x3cc97c, 0x35fff9e9),
         0xa3: (0xabcca4, 0xa9177e7f)}

def word(blob, at):
    if at < 0 or at + 4 > len(blob):
        raise ValueError('PC item gate outside image')
    return struct.unpack_from('<I', blob, at)[0]

def branch(at, to):
    delta = to - at
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('PC item branch outside range')
    return 0x14000000 | ((delta >> 2) & 0x3ffffff)

def adrp(reg, at, to):
    delta = ((to & ~4095) - (at & ~4095)) >> 12
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError('PC item ADRP outside range')
    return 0x90000000 | reg | ((delta & 3) << 29) | (((delta >> 2) & 0x7ffff) << 5)

def verify(mod, original, patched, header, gates, cuts):
    if mod != 'main':
        return set()
    seen = set()
    for number, (site, native) in SITES.items():
        matches = [row for row in cuts if row[0] == site]
        if not matches:
            continue
        if len(matches) != 1:
            raise ValueError('Duplicate PC item cut')
        _, before, after, _ = matches[0]
        if native is None:
            native = adrp(8, site, 0x2d12000) if number == 0x9f else branch(site, 0x2ab480) | 0x80000000
        head = word(patched, site)
        if len(before) != 4 or len(after) != 4 or before != struct.pack('<I', native) or word(original, site) != native or after != struct.pack('<I', head) or head & 0xfc000000 != 0x14000000:
            raise ValueError(f'PC item svc{number:x}: invalid cut/preimage')
        signed = ((head & 0x3ffffff) ^ 0x2000000) - 0x2000000
        start = site + signed * 4
        if start < header['PATCH_OFF'] or start % 4:
            raise ValueError('PC item gate is not in patch area')
        expected = []
        def emit(value):
            expected.append(value)
        def pc():
            return start + len(expected) * 4
        def jump(to):
            emit(branch(pc(), to))
        if number != 0xa1:
            emit(0xa9bf7fe8)
        emit(0xa9bf7bfd)
        if number == 0xa2:
            emit(0xaa1e03e8)
        emit(0xd53bd07e)
        emit(adrp(29, pc(), header['KEY_PAGE_OFF']))
        key = header['KEY_KGATE_IDX']
        if not 0 <= key < 8:
            raise ValueError('Invalid KGATE slot')
        emit(0xf94003bd | (key << 10))
        emit(0x8b1d03de)
        emit(0xf94003de)
        emit(0xf94003fd)
        emit(0xd63f03c0)
        return_pc = pc()
        emit(0xa8c17bfd)
        if number == 0xa1:
            emit(adrp(30, pc(), site + 4))
            emit(0x910003de | (((site + 4) & 4095) << 10))
            jump(0x2ab480)
        elif number == 0xa2:
            emit(0xa8c17fe8)
            emit(0x34000049)
            jump(0x3cc8b8)
            jump(site + 4)
        else:
            emit(0x34000068)
            emit(0xa8c17fe8)
            jump({0x9d: 0x3ca590, 0x9e: site + 4, 0x9f: 0x3b8664,
                  0xa0: 0x3db198, 0xa3: 0xabccd8}[number])
            emit(0xa8c17fe8)
            if number == 0x9e:
                emit(adrp(30, pc(), site + 4))
                emit(0x910003de | (((site + 4) & 4095) << 10))
                emit(0xd61f0100)
            elif number in (0x9d, 0xa0):
                emit(0x54000041 if number == 0x9d else 0x36000040)
                jump(0x3ca5a0 if number == 0x9d else 0x3db2b4)
                jump(site + 4)
            else:
                emit(adrp(8, pc(), 0x2d12000) if number == 0x9f else native)
                jump(site + 4)
        actual = [word(patched, start + 4*i) for i in range(len(expected))]
        if actual != expected or gates.get(return_pc) != ('svc', number, site):
            raise ValueError(f'PC item svc{number:x}: stub or gates.tsv mismatch')
        seen.add(return_pc)
    return seen
