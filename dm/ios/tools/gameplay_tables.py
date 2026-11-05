"""构建时调整普通宝箱和消耗品掉落。

3.3.94012 的序列化表以 u32 条数开头，后接定长记录。
改 BIN 前先用配套 TXT 核对目标字段；游戏实际读取 BIN。
仓库不分发原版游戏表。

Build-time changes to ordinary chests and consumable drops.

3.3.94012 serialized tables have a u32 count followed by fixed-size records.
Validate the edited fields against the companion TXT before changing BIN, which
is what the game loads. Never distribute copies of the original game tables.
"""
from pathlib import Path
import struct


def _read(text, binary, size):
    if len(binary) < 4 or len(binary) != 4 + struct.unpack_from('<I', binary)[0] * size:
        raise ValueError('Unsupported gameplay table record size')
    lines = text.decode('utf-8-sig').replace('\r\n', '\n').split('\n')
    header = lines[0].split('\t')
    return [line.split('\t') for line in lines], {c: i for i, c in enumerate(header)}


def _text(rows):
    return '\n'.join('\t'.join(row) for row in rows).encode('utf-8')


def _check(actual, expected):
    if actual != expected:
        raise ValueError(f'Gameplay TXT/BIN mismatch: {actual!r} != {expected!r}')


def unlock_chests(text, binary):
    rows, cols = _read(text, binary, 360)
    out = bytearray(binary)
    count = seen = 0
    for row in rows[1:]:
        if len(row) <= cols['*ID'] or not row[cols['*ID']].isdigit():
            continue  # 末尾 *eol 行不是记录 | trailing *eol row is not a record
        # *ID 是说明列；资料片记录没有按 ID 排列。
        # *ID is a documentation column; expansion rows are not in ID order.
        index = seen
        seen += 1
        base = 4 + index * 360
        for col, offset in (('Lockable', 0x12f), ('OperateFn', 0x15e), ('InitFn', 0x15c)):
            _check(binary[base + offset], int(row[cols[col]] or 0))
        # Operate 4 = 普通宝箱；Init 3 / 57 = 普通 / 闪光。
        # 魔盒、卷轴、法杖、卡林任务箱及墨菲斯托桥使用其他函数组合。
        # Operate 4 = ordinary chest; init 3 / 57 = normal / sparkly.
        # Cube, scroll, staff, Khalim chests and Mephisto bridge use other pairs.
        if row[cols['Lockable']] == '1' and row[cols['OperateFn']] == '4' and row[cols['InitFn']] in ('3', '57'):
            row[cols['Lockable']] = '0'
            out[base + 0x12f] = 0
            count += 1
    _check(seen, struct.unpack_from('<I', binary)[0])
    return _text(rows), bytes(out), count


def remove_consumables(text, binary):
    rows, cols = _read(text, binary, 772)
    out = bytearray(binary)
    count = seen = 0
    for row in rows[1:]:
        if not row[0]:
            continue
        base = 4 + seen * 772
        seen += 1
        _check(binary[base:base+48].split(b'\0')[0], row[cols['Treasure Class']].encode()[:47])
        kept = []
        removed = 0
        for i in range(10):
            item, prob = row[cols[f'Item{i+1}']], row[cols[f'Prob{i+1}']]
            offset = base + 0x48 + i * 64
            _check(binary[offset:offset+64].split(b'\0')[0].decode(), item)
            _check(struct.unpack_from('<i', binary, base+0x2c8+i*4)[0], int(prob or 0))
            if item in ('key', 'tsc', 'isc'):
                removed += 1
            elif item:
                kept.append((item, prob))
        if not removed:
            continue
        count += removed
        # 压紧保留项，兼容遇到第一个空槽就停止的加载器。
        # Compact survivors so loaders that stop at the first empty slot work.
        for i, (item, prob) in enumerate(kept + [('', '')] * (10 - len(kept))):
            row[cols[f'Item{i+1}']], row[cols[f'Prob{i+1}']] = item, prob
            offset = base + 0x48 + i * 64
            out[offset:offset+64] = item.encode().ljust(64, b'\0')
            struct.pack_into('<i', out, base+0x2c8+i*4, int(prob or 0))
    _check(seen, struct.unpack_from('<I', binary)[0])
    return _text(rows), bytes(out), count


def build(romfs, output):
    import romfs_art
    replacements = {}
    for name, transform in (('objects', unlock_chests), ('treasureclassex', remove_consumables)):
        prefix = 'data/global/excel/' + name
        text, binary, count = transform(romfs_art.read(romfs, prefix+'.txt'),
                                        romfs_art.read(romfs, prefix+'.bin'))
        for extension, payload in (('.txt', text), ('.bin', binary)):
            path = Path(output) / (name + extension)
            path.write_bytes(payload)
            replacements[prefix+extension] = str(path)
        print(f'gameplay: {name}: {count} changes (TXT + BIN)')
    return replacements
