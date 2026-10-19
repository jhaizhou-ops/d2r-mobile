"""Independent word templates for the 3.3.94012 PC input gates.

The A3 template is shared with the optional inventory verifier. Returned PCs
are unioned by structck, so a combined product counts that gate only once.
"""
import struct

CALLS = {0x38f578: (0x8f, 0x36fe60), 0x3e3c48: (0x90, 0x2b3d68),
         0xbb6f2c: (0x91, 0xaf3d68), 0xaecca4: (0x95, 0xada890),
         0xbb59b4: (0x98, 0xae5b3c), 0xc26e0c: (0xa4, 0xc2569c)}
BRANCHES = {0xb0d150: (0x92, 0x911dc108, 0xb0d168),
            0xad8a54: (0x93, 0xd10383ff, 0xad8a58),
            0x456c5c: (0x94, 0x911bf7e8, 0x456d50),
            0x368d64: (0x96, 0xd288a588, 0x368e10),
            0xae950c: (0x97, 0xf9422ee8, 0xae9510),
            0xae5324: (0x9c, 0xd288a588, 0xae53d0),
            0xabcca4: (0xa3, 0xa9177e7f, 0xabccd8),
            0x3e2f6c: (0xa6, 0x2a1703e0, 0x3e2f80),
            0xc065a4: (0xa7, 0xb94087eb, 0xc066b0)}
APPENDS = (0xc068f4, 0xc06908, 0xc0691c)


def word(blob, at):
    if at < 0 or at + 4 > len(blob):
        raise ValueError('PC input gate outside image')
    return struct.unpack_from('<I', blob, at)[0]


def branch(at, to):
    delta = to - at
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('PC input branch outside range')
    return 0x14000000 | ((delta >> 2) & 0x3ffffff)


def adrp(reg, at, to):
    delta = ((to & ~4095) - (at & ~4095)) >> 12
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError('PC input ADRP outside range')
    return 0x90000000 | reg | ((delta & 3) << 29) | (((delta >> 2) & 0x7ffff) << 5)


def verify(mod, original, patched, header, gates, cuts):
    if mod != 'main':
        return set()
    contracts = dict(BRANCHES)
    contracts.update({site: (number, branch(site, target) | 0x80000000, None)
                      for site, (number, target) in CALLS.items()})
    contracts[0x3e4760] = (0xa5, 0xd65f03c0, None)
    contracts.update({site: (0xa8, branch(site, 0xc08cc8) | 0x80000000, None)
                      for site in APPENDS})
    seen = set()
    for site, (number, native, target) in contracts.items():
        matches = [row for row in cuts if row[0] == site]
        if not matches:
            continue
        if len(matches) != 1:
            raise ValueError('Duplicate PC input cut')
        _, before, after, _ = matches[0]
        head = word(patched, site)
        is_call = site in CALLS
        expected_cut = struct.pack('<I', 0xd4000001 | (number << 5) if is_call else head)
        if before != struct.pack('<I', native) or word(original, site) != native or after != expected_cut or head & 0xfc000000 != 0x14000000:
            raise ValueError(f'PC input svc{number:x}: invalid cut/preimage')
        start = site + 4 * (((head & 0x3ffffff) ^ 0x2000000) - 0x2000000)
        if start < header['PATCH_OFF'] or start % 4:
            raise ValueError('PC input gate is not in patch area')
        key = header['KEY_KGATE_IDX']
        if not 0 <= key < 8:
            raise ValueError('Invalid KGATE slot')
        expected = []
        def emit(value):
            expected.append(value)
        def pc():
            return start + len(expected) * 4
        def jump(to):
            emit(branch(pc(), to))
        if not is_call and number != 0xa5:
            emit(0xa9bf7fe8)
        emit(0xa9bf7bfd)
        emit(0xd53bd07e)
        emit(adrp(29, pc(), header['KEY_PAGE_OFF']))
        emit(0xf94003bd | (key << 10))
        emit(0x8b1d03de)
        emit(0xf94003de)
        emit(0xf94003fd)
        emit(0xd63f03c0)
        return_pc = pc()
        emit(0xa8c17bfd)
        if is_call:
            jump(site + 4)
        elif number == 0xa5:
            emit(0xd65f03c0)
        elif number == 0xa8:
            emit(0x340000a8)
            for destination in (site + 4, 0xc08cc8):
                emit(0xa8c17fe8)
                emit(adrp(30, pc(), site + 4))
                emit(0x910003de | (((site + 4) & 4095) << 10))
                jump(destination)
        else:
            emit(0x34000068)
            emit(0xa8c17fe8)
            jump(target)
            emit(0xa8c17fe8)
            emit(native)
            jump(site + 4)
        if [word(patched, start + 4*i) for i in range(len(expected))] != expected or gates.get(return_pc) != ('svc', number, site):
            raise ValueError(f'PC input svc{number:x}: stub or gates.tsv mismatch')
        seen.add(return_pc)
    return seen
