"""Input-only fixtures must stay disconnected when the optional item header exists."""
from pathlib import Path
import subprocess
import tempfile
import unittest


TESTS = Path(__file__).resolve().parent
CURSOR = TESTS.parent / 'dm_cursor.c'


class OptionalItems(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='pc-items-boundary-')
        self.addCleanup(self.directory.cleanup)
        self.include = Path(self.directory.name)
        (self.include / 'dm_pc_items.h').write_text('#pragma once\n', encoding='utf-8')

    def clang(self, *arguments):
        result = subprocess.run(
            ['clang', '-I' + str(self.include), *map(str, arguments)],
            cwd=TESTS, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_production_keeps_default_autodetection(self):
        macros = self.clang('-E', '-dM', CURSOR)
        self.assertIn('#define DM_PC_ITEMS_CONNECTED 1\n', macros)

    def test_explicit_disconnect_does_not_include_item_header(self):
        macros = self.clang('-E', '-dM', '-DDM_PC_ITEMS_CONNECTED=0', CURSOR)
        self.assertIn('#define DM_PC_ITEMS_CONNECTED 0\n', macros)
        expanded = self.clang('-E', '-DDM_PC_ITEMS_CONNECTED=0', CURSOR)
        self.assertNotRegex(expanded, r'(?m)^# \d+ "[^"\n]*[/\\]dm_pc_items\.h"')

    def test_frontend_fixture_declares_its_input_only_configuration(self):
        (self.include / 'dm_pc_items.h').write_text(
            '#error The input fixture must not include the item bridge\n', encoding='utf-8')
        self.clang('-fsyntax-only', '-Wall', '-Werror', '-Wno-unused-function',
                   TESTS / 't_pc_frontend.c')


if __name__ == '__main__':
    unittest.main()
