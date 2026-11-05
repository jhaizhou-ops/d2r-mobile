"""合成表夹具：测试不分发任何游戏资源。 | Synthetic table fixtures: no game assets are distributed with these tests."""
import csv
import io
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import gameplay_tables as tables


def tsv(columns, rows):
    stream = io.StringIO()
    writer = csv.writer(stream, delimiter='\t', lineterminator='\n')
    writer.writerow(columns)
    writer.writerows(rows)
    return stream.getvalue().encode()


class GameplayTablesTest(unittest.TestCase):
    def test_unlocks_only_standard_chests_and_preserves_quest_objects(self):
        values = [(4, 3), (4, 57), (39, 31), (40, 32), (41, 33),
                  (57, 0), (59, 0), (58, 0), (4, 45), (46, 44)]
        text = tsv(['*ID', 'Lockable', 'OperateFn', 'InitFn'],
                   [(i, 1, op, init) for i, (op, init) in enumerate(values)])
        binary = bytearray(struct.pack('<I', len(values)) + bytes(360 * len(values)))
        for i, (op, init) in enumerate(values):
            base = 4 + i * 360
            binary[base + 0x12f] = 1
            binary[base + 0x15e] = op
            binary[base + 0x15c] = init
        new_text, new_binary, count = tables.unlock_chests(text, bytes(binary))
        self.assertEqual(count, 2)
        rows = list(csv.DictReader(io.StringIO(new_text.decode()), delimiter='\t'))
        self.assertEqual([r['Lockable'] for r in rows], ['0', '0'] + ['1'] * 8)
        expected = bytearray(binary)
        for i in (0, 1):
            expected[4 + i * 360 + 0x12f] = 0
        self.assertEqual(new_binary, expected)
        self.assertEqual(tables.unlock_chests(new_text, new_binary), (new_text, new_binary, 0))
        binary[4 + 0x15e] = 39
        with self.assertRaises(ValueError):
            tables.unlock_chests(text, bytes(binary))

    def test_removes_consumables_and_keeps_quest_keys_and_other_drop_weights(self):
        items = ['key', 'pk1', 'tsc', 'isc', 'pk2', 'pk3', 'qey', 'mp1', '', '']
        weights = [3, 2, 3, 4, 1, 1, 1, 6, 0, 0]
        cols = ['Treasure Class', 'Picks', 'NoDrop', 'QuestFlag']
        for i in range(1, 11):
            cols += [f'Item{i}', f'Prob{i}']
        text = tsv(cols, [['Test', 1, 7, 2] +
                          [v for pair in zip(items, weights) for v in pair]])
        binary = bytearray(struct.pack('<I', 1) + bytes(772))
        binary[4:8] = b'Test'
        for i, (item, weight) in enumerate(zip(items, weights)):
            base = 4 + 0x48 + i * 64
            binary[base:base + len(item)] = item.encode()
            struct.pack_into('<i', binary, 4 + 0x2c8 + i * 4, weight)
        new_text, new_binary, count = tables.remove_consumables(text, bytes(binary))
        self.assertEqual(count, 3)
        row = next(csv.DictReader(io.StringIO(new_text.decode()), delimiter='\t'))
        kept = [('pk1', 2), ('pk2', 1), ('pk3', 1), ('qey', 1), ('mp1', 6)]
        self.assertEqual(row['NoDrop'], '7')
        self.assertEqual(row['QuestFlag'], '2')
        for i, (item, weight) in enumerate(kept + [('', 0)] * 5):
            self.assertEqual(row[f'Item{i+1}'], item)
            self.assertEqual(int(row[f'Prob{i+1}'] or 0), weight)
            base = 4 + 0x48 + i * 64
            self.assertEqual(new_binary[base:base+64].split(b'\0')[0].decode(), item)
            self.assertEqual(struct.unpack_from('<i', new_binary, 4+0x2c8+i*4)[0], weight)
        self.assertEqual(new_binary[:4+0x48], binary[:4+0x48])
        self.assertEqual(new_binary[4+0x2f0:], binary[4+0x2f0:])
        self.assertEqual(tables.remove_consumables(new_text, new_binary), (new_text, new_binary, 0))
        struct.pack_into('<i', binary, 4 + 0x2c8, 99)
        with self.assertRaises(ValueError):
            tables.remove_consumables(text, bytes(binary))

    def test_rejects_unknown_binary_layout(self):
        with self.assertRaises(ValueError):
            tables.unlock_chests(tsv(['*ID'], [[0]]), struct.pack('<I', 1))


if __name__ == '__main__':
    unittest.main()
