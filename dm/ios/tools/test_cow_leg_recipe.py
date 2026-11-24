# 牛门配方修改原型回归：全为自造表，检查只改单元格与不认识时拒绝。
# Cow recipe prototype regressions: authored tables only; verify single-cell scope and refusal of unknown formats.
import unittest
import subprocess
import sys
import tempfile
from pathlib import Path
from cow_leg_recipe import patch_recipe

HEADER = 'enabled\tnuminputs\top\tparam\tvalue\tinput 1\tinput 2\tinput 3\toutput\toutput b\tversion'
COW = '1\t2\t28\t\t\tleg\ttbk\t\tCow Portal\t\t100'
OTHER = '1\t3\t28\t\t\tkey_a\tkey_b\tkey_c\tOther Portal\t\t100'


def table(row=COW, end='\n'):
    return end.join([HEADER, OTHER, row, '']).encode()


class CowRecipeTests(unittest.TestCase):
    def test_one_cell_only_and_idempotence(self):
        for end in ('\n', '\r\n'):
            for bom in (b'', b'\xef\xbb\xbf'):
                with self.subTest(end=end, bom=bom):
                    source = bom + table(end=end)
                    expected = bom + table(COW.replace('\t28\t', '\t\t', 1), end=end)
                    self.assertEqual(patch_recipe(source), expected)
                    self.assertEqual(patch_recipe(expected), expected)

    def test_reversed_inputs(self):
        source = table(COW.replace('leg\ttbk', 'tbk\tleg'))
        self.assertEqual(patch_recipe(source), source.replace(b'2\t28\t', b'2\t\t', 1))

    def test_reject_unknown_conditions_and_rows(self):
        cases = [b'', table().replace(b'op\t', b'unknown\t'), table(COW.replace('\t28\t', '\t27\t')),
                 table(COW.replace('\t28\t\t\t', '\t28\t356\t1\t')), table(COW.replace('leg\ttbk', 'leg\tisc')),
                 table(COW.replace('leg\ttbk\t\t', 'leg\ttbk\tkey\t')), table(COW.replace('1\t2\t', '1\t3\t')),
                 table(COW.replace('Cow Portal\t\t', 'Cow Portal\titem\t')), table(COW.replace('Cow Portal', 'Other')),
                 table() + (COW+'\n').encode(), table() + b'bad\trow\n', table().replace(b'enabled\t', b'op\t')]
        for source in cases:
            with self.subTest(source=source):
                with self.assertRaises(ValueError):
                    patch_recipe(source)

    def test_cli_never_overwrites_input_or_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source.txt'
            output = Path(directory) / 'output.txt'
            source.write_bytes(table())
            script = Path(__file__).with_name('cow_leg_recipe.py')
            def run(target):
                return subprocess.run([sys.executable, str(script), str(source), str(target)], capture_output=True)
            self.assertNotEqual(run(source).returncode, 0)
            self.assertEqual(source.read_bytes(), table())
            self.assertEqual(run(output).returncode, 0)
            before = output.read_bytes()
            self.assertNotEqual(run(output).returncode, 0)
            self.assertEqual(output.read_bytes(), before)
            self.assertEqual(source.read_bytes(), table())


if __name__ == '__main__':
    unittest.main()
