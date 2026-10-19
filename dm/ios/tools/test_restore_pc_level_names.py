"""Delta de nombres de zonas: fuente conocida, 13 idiomas y rechazo de cambios ajenos."""
import copy
import os
from pathlib import Path
import unittest
import tempfile
import json
import restore_pc_level_names as restore

ROMFS = Path(str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS'))
ES16 = Path(os.environ.get('DM_TEST_LEVEL_REFERENCE', '/nonexistent/reference'))

@unittest.skipUnless(ROMFS.is_dir(), 'Requiere RomFS propio')
class LevelNames(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw, cls.levels = restore.source(ROMFS)
        cls.rows = restore.parse(cls.raw)
        cls.annotated = restore.expected_annotations(cls.rows, cls.levels)

    @unittest.skipUnless('DM_TEST_LEVEL_REFERENCE' in os.environ, 'Comparación histórica optativa')
    def test_known_generated_source_matches_all_thirteen_languages(self):
        raw = restore.pc.read_overlay(ROMFS, Path(str(ES16) + '_variants/item_notes'), restore.LOGICAL)
        self.assertEqual(restore.validate_source(self.raw, raw, self.levels), {'zhCN': 118, 'esES': 118, 'esMX': 118})
        self.assertEqual(len(self.rows), 422)
        self.assertEqual(len(restore.LANGS), 13)

    def test_unrelated_name_change_rejected(self):
        changed = copy.deepcopy(self.annotated)
        changed[0]['enUS'] += ' personaje propio'
        with self.assertRaises(ValueError): restore.validate_rows(self.rows, changed, self.levels)

    def test_unknown_or_doubled_numeric_suffix_rejected(self):
        changed = copy.deepcopy(self.annotated)
        index = next(i for i, row in enumerate(changed) if row['esES'] != self.rows[i]['esES'])
        changed[index]['esES'] += '[2/36/68]'
        with self.assertRaises(ValueError): restore.validate_rows(self.rows, changed, self.levels)

    def test_keys_ids_order_and_nonannotated_languages_rejected(self):
        for field, value in (('Key', 'otra clave'), ('id', -1), ('frFR', 'Otro nombre')):
            changed = copy.deepcopy(self.annotated)
            changed[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): restore.validate_rows(self.rows, changed, self.levels)

    def test_generated_replacements_restore_only_known_level_resource(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            annotated = work / 'annotated.json'
            annotated.write_text(json.dumps(self.annotated, ensure_ascii=False), encoding='utf-8-sig')
            other = work / 'other.bin'
            other.write_bytes(b'preservado')
            replacements = {restore.LOGICAL: str(annotated), 'other': str(other)}
            before = annotated.read_bytes()
            result, counts = restore.restore_generated(ROMFS, work, replacements)
            self.assertEqual(Path(result[restore.LOGICAL]).read_bytes(), self.raw)
            self.assertEqual(result['other'], str(other))
            self.assertEqual(other.read_bytes(), b'preservado')
            self.assertEqual(annotated.read_bytes(), before)
            self.assertEqual(replacements[restore.LOGICAL], str(annotated))
            self.assertEqual(counts, {'zhCN': 118, 'esES': 118, 'esMX': 118})

    def test_generated_replacements_reject_unrelated_edits(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            annotated = work / 'annotated.json'
            wrong = copy.deepcopy(self.annotated)
            wrong[0]['esES'] += ' ajeno'
            annotated.write_text(json.dumps(wrong, ensure_ascii=False), encoding='utf-8-sig')
            with self.assertRaises(ValueError):
                restore.restore_generated(ROMFS, work, {restore.LOGICAL: str(annotated)})

    @unittest.skipUnless('DM_TEST_LEVEL_REFERENCE' in os.environ, 'Comparación histórica optativa')
    def test_clean_base_is_already_exact_original(self):
        self.assertEqual(restore.pc.read_overlay(ROMFS, ES16, restore.LOGICAL), self.raw)

if __name__ == '__main__': unittest.main()
