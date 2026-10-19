"""Notas españolas generadas frente al parser ARM original 3.3.94012."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tempfile
import unittest

import annot_strings as annot
import touch_layout

ROMFS = Path(os.environ.get('DM_TEST_ROMFS', str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS')))
PREVIOUS_NOTES = Path(os.environ.get('DM_TEST_PREVIOUS_NOTES', '/nonexistent/reference'))
BINARY = Path('dm/host/tests/build/main.orig.bin')
if 'DM_TEST_ROMFS' in os.environ and (not ROMFS.is_dir() or not BINARY.is_file()):
    raise RuntimeError('DM_TEST_ROMFS requiere RomFS existente y dm/host/tests/build/main.orig.bin')
if 'DM_TEST_PREVIOUS_NOTES' in os.environ and not PREVIOUS_NOTES.is_dir():
    raise RuntimeError('DM_TEST_PREVIOUS_NOTES no existe: ' + str(PREVIOUS_NOTES))


def native_name(value):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn import arm64_const as r
    binary = BINARY.read_bytes()
    assert hashlib.sha256(binary).hexdigest() == '6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0, 0x4000000); u.mem_write(0, binary)
    u.mem_map(0x5000000, 0x100000)
    stack, target = 0x5000000, 0x5080000
    source = stack + 0x1078
    encoded = value.encode('utf-8')
    u.mem_write(source, encoded + b'\0')
    def readz(at):
        out = bytearray()
        while (c := bytes(u.mem_read(at + len(out), 1))) != b'\0': out += c
        return bytes(out)
    def hook(cpu, at, size, data):
        if at == 0x1b76070:
            u.reg_write(r.UC_ARM64_REG_X0, len(readz(u.reg_read(r.UC_ARM64_REG_X0))))
        elif at == 0x1b76040:
            dst, src, n = [u.reg_read(reg) for reg in (r.UC_ARM64_REG_X0, r.UC_ARM64_REG_X1, r.UC_ARM64_REG_X2)]
            if n: u.mem_write(dst, bytes(u.mem_read(src, n)))
        else: return
        u.reg_write(r.UC_ARM64_REG_PC, u.reg_read(r.UC_ARM64_REG_LR))
    u.hook_add(UC_HOOK_CODE, hook)
    for reg, value in ((r.UC_ARM64_REG_SP, stack), (r.UC_ARM64_REG_X23, source),
                       (r.UC_ARM64_REG_X24, len(encoded)), (r.UC_ARM64_REG_X28, target)):
        u.reg_write(reg, value)
    u.emu_start(0x41a50c, 0x41c77c, count=10000)
    assert u.reg_read(r.UC_ARM64_REG_PC) == 0x41c77c
    return readz(target).decode('utf-8')


@unittest.skipUnless(ROMFS.is_dir() and BINARY.is_file(), 'Requiere RomFS y binario original locales')
class SpanishItemNotes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        with contextlib.redirect_stdout(io.StringIO()):
            cls.repl, cls.stats = annot.build(str(ROMFS), cls.tmp.name, {})
        cls.rows = {row['Key']: row for row in json.loads(Path(cls.repl['data/local/lng/strings/item-names.json']).read_text(encoding='utf-8-sig'))}
        _, raw = touch_layout.read_layout(str(ROMFS), '', suffix='/lng/strings/item-names.json')
        cls.original = {row['Key']: row for row in json.loads(raw)}
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def test_generated_dagger_keeps_tier_without_grammar_leak(self):
        self.assertEqual(native_name('[fs]Objeto[normal]'), '[fs]Objeto[normal]')
        for lang in ('esES', 'esMX'):
            self.assertEqual(native_name(self.rows['dgr'][lang]), annot.collection_native_text(self.original['dgr'][lang]) + ' (normal)')
    def test_generated_crafted_gloves_keep_note_and_tier(self):
        for lang in ('esES', 'esMX'):
            self.assertEqual(native_name(self.rows['vgl'][lang]), '(Artesanía: golpe triturador)\n' + annot.collection_native_text(self.original['vgl'][lang]) + ' (normal)')
    def test_generated_elite_armor_keeps_value_star_and_tier(self):
        for lang in ('esES', 'esMX'):
            value = native_name(self.rows['xtp'][lang])
            self.assertRegex(value, r'Defensa [0-9]+-[0-9]+')
            self.assertIn('★', value)
            self.assertIn(annot.collection_native_text(self.original['xtp'][lang]), value)
            self.assertIn('(excep.·ligera)', value)
            self.assertNotIn('[fs]', value)
    def test_real_notes_preserve_other_languages_and_all_metadata(self):
        import pc_desktop_layout as pc
        previous = PREVIOUS_NOTES
        if not previous.is_dir(): self.skipTest('Requiere snapshot anterior es27')
        for logical, path in self.repl.items():
            before = pc.read_overlay(ROMFS, previous, logical)
            after = Path(path).read_bytes()
            if logical.endswith('levels.json'):
                continue
            mask = lambda b: re.sub(rb'("es(?:ES|MX)":\s*)"(?:[^"\\]|\\.)*"', rb'\1"<ES>"', b)
            self.assertEqual(mask(before), mask(after), logical)
            old = {r['Key']: r for r in json.loads(before.decode('utf-8-sig'))}
            for row in json.loads(after.decode('utf-8-sig')):
                for lang in ('esES', 'esMX'):
                    if row[lang] == old[row['Key']][lang]: continue
                    tag = re.findall(r'\[[mf][sp]\]', old[row['Key']][lang])
                    self.assertEqual(len(tag), 1)
                    self.assertTrue(row[lang].startswith(tag[0]))
                    payload = lambda t: re.sub(r'[\[\]()\s]', '', re.sub(r'\[[mf][sp]\]', '', t))
                    self.assertEqual(payload(row[lang]), payload(old[row['Key']][lang]))
    def test_official_grammar_variants_are_preserved_or_rejected(self):
        original = '[fs]sintética[ms]sintético[fp]sintéticas[mp]sintéticos'
        self.assertEqual(annot.spanish_annotated_name(original, original), original)
        with self.assertRaises(ValueError): annot.spanish_annotated_name(original, original + '[normal]')
        with self.assertRaises(ValueError): annot.spanish_annotated_name('[fs]Objeto', '[fs][fs]Objeto[normal]')
    def test_class_template_changes_only_esES_and_keeps_all_eight_classes(self):
        logical, original = touch_layout.read_layout(str(ROMFS), None, suffix='/lng/strings/item-modifiers.json')
        repl = annot.spanish_class_skill_template(str(ROMFS), self.tmp.name, {})
        actual = Path(repl[logical.lower()]).read_bytes().decode('utf-8-sig')
        self.assertEqual(original.replace('"esES": "%+d a %s (%s)"', '"esES": "%+d a %s %s"'), actual)
        rows = {r['Key']: r for r in json.loads(actual)}
        for key in ('AmaOnly', 'SorOnly', 'NecOnly', 'PalOnly', 'BarOnly', 'DruOnly', 'AssOnly', 'WarOnly'):
            result = rows['ItemModifierClassSkill']['esES'] % (1, 'Habilidad sintética', rows[key]['esES'])
            self.assertEqual(result.count('('), 1)
            self.assertEqual(result.count(')'), 1)
        with self.assertRaises(AssertionError): annot.spanish_class_skill_template(str(ROMFS), self.tmp.name, repl)


if __name__ == '__main__': unittest.main()
