"""Input-only PC overlay, using a small generated RomFS without game assets."""
import hashlib
import json
import os
import runpy
from pathlib import Path
import struct
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import pc_desktop_layout as pc
import gen_mac_cursors
import dm_i18n
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import casc_overlay as casc


class InputOverlay(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='pc-input-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.romfs = self.root / 'romfs'
        self.out = self.root / 'out'
        (self.romfs / 'data').mkdir(parents=True)
        self.out.mkdir()
        self.files = {
            'data/local/lng/strings/ui.json': json.dumps([{'id':7,'Key':'SyntheticOriginal',
                **{lang:'original-'+lang for lang in dm_i18n.LANGS}}]).encode(),
            pc.LAY + '_profilehd.json': b'{"SettingsPanelTabsRect":{"width":900},"Font":24}',
            pc.LAY + '_profilelv.json': b'{"basedOn":"HD","Font":42}',
            pc.LAY + '_profileasian.json': b'{"basedOn":"HD","Font":30}',
            pc.LAY + 'characterselectpanelhd.json': json.dumps({
                'name': 'CharacterSelect', 'children': [{'name': 'CreateCharacter', 'fields': {
                    'onClickMessage': 'FrontEndNavigation:ToCharacterCreate'}}]}).encode(),
            pc.LAY + 'controller/controlleroverlayhd.json': json.dumps({
                'name': 'ControllerOverlay', 'children': [{'name': name, 'fields': {
                    'fitToParent': True, 'rect': {'x': 0, 'y': 0}}} for name in ('TransparentBG', 'Anchor')]}).encode(),
        }
        for family in pc.CONTROLLER_PROFILES:
            self.files[pc.LAY + 'controller/_profile' + family + '.json'] = b'{"basedOn":"Controller","Font":99,"PlatformOnly":7}'
        for name in ('idle', 'hold', 'held'):
            raw = bytearray(56)
            raw[:4] = b'SpA1'
            struct.pack_into('<H', raw, 4, 61)
            struct.pack_into('<I', raw, 32, 16)
            raw[40:] = b'\xff' * 16
            self.files['data/hd/global/ui/controller/cursor/' + name + '.sprite'] = bytes(raw)
        for name, hotspot in (('protate', (3, 14)), ('ppress', (4, 14))):
            raw = bytearray(56)
            raw[:4] = b'SpA1'
            struct.pack_into('<HHIIhhH', raw, 4, 61, 69, 138, 64, *hotspot, 2)
            self.files['data/hd/global/ui/cursor/' + name + '.sprite'] = bytes(raw)
        # Original PC roots are resolved by OBS24/25 and must stay untouched.
        for name in ('hudpanelhd', 'charactercreatepanelhd', 'bankexpansionlayouthd', 'npctalkpanelhd'):
            self.files[pc.LAY + name + '.json'] = b'{"original":true}'
        manifest, archive, records = [], bytearray(), []
        for logical, raw in self.files.items():
            key = hashlib.md5(raw).digest()[:9] + bytes(2) + len(archive).to_bytes(5, 'big')
            fields = ['', '', '', '', hashlib.md5(raw).hexdigest(), '', key.hex(), str(len(raw)), 'n', 'data:' + logical, '']
            manifest.append('|'.join(fields))
            archive.extend(raw)
            records.append((logical, key, len(raw)))
        (self.romfs / 'data/build_manifest.psv').write_text('\n'.join(manifest) + '\n')
        # A complete synthetic TVFS exercises the actual packer and reader.
        width = casc.offset_field_size(25 * len(records))
        names = {name.encode(): i * (9 + width) for i, (name, _, _) in enumerate(records)}
        paths, depth = casc.trie_bytes(names)
        specs = b'n\0'
        containers = b''.join(key + struct.pack('>I', size) + b'\0' + struct.pack('>I', size) for _, key, size in records)
        entries = b''.join(b'\1' + struct.pack('>II', 0, size) + (25 * i).to_bytes(width, 'big') for i, (_, _, size) in enumerate(records))
        head = bytearray(46)
        head[:4] = b'TVFS'
        struct.pack_into('>IIIIII', head, 12, 46, len(paths), 46 + len(paths) + len(specs) + len(containers), len(entries), 46 + len(paths) + len(specs), len(containers))
        struct.pack_into('>HII', head, 36, depth, 46 + len(paths), len(specs))
        vfs = bytes(head) + paths + specs + containers + entries
        def append(raw):
            encoded = casc.blte_encode(raw)
            key = casc.ekey_for(encoded, 0, 0, len(archive))
            archive.extend(encoded)
            return hashlib.md5(raw).hexdigest() + ' ' + key.hex(), str(len(raw)) + ' ' + str(len(encoded)), key
        vfs_value, vfs_size, vfs_key = append(vfs)
        root = bytearray(80)
        root[:4] = b'TVFS'
        struct.pack_into('>I', root, 42, 1)
        root[48:64] = vfs_key
        root_value, root_size, _ = append(bytes(root))
        (self.romfs / 'data/.build.config').write_text('vfs-1 = ' + vfs_value + '\nvfs-1-size = ' + vfs_size + '\nvfs-root = ' + root_value + '\nvfs-root-size = ' + root_size + '\n')
        (self.romfs / 'data/00-00000000.data').write_bytes(archive)

    def test_input_layouts_only_and_native_handlers(self):
        repl = pc.build_layouts(self.romfs, self.out)
        expected = {pc.LAY + 'controller/_profile' + f + '.json' for f in pc.CONTROLLER_PROFILES}
        expected |= {pc.LAY + 'controller/controlleroverlayhd.json', pc.LAY + 'characterselectpanelhd.json'}
        self.assertEqual(set(repl), expected)
        select = json.loads(Path(repl[pc.LAY + 'characterselectpanelhd.json']).read_text())
        self.assertEqual(pc.find(select, 'CreateCharacter')['fields']['onClickMessage'], 'FrontEndNavigation:ToCharacterCreate:1')
        for family in pc.CONTROLLER_PROFILES:
            profile = json.loads(Path(repl[pc.LAY + 'controller/_profile' + family + '.json']).read_text())
            self.assertEqual(profile['Font'], 30 if family == 'asian' else 42 if family.endswith('lv') else 24)
            self.assertEqual(profile['PlatformOnly'], 7)
            self.assertEqual(profile['basedOn'], 'Controller')
        shell = json.loads(Path(repl[pc.LAY + 'controller/controlleroverlayhd.json']).read_text())
        self.assertEqual(pc.find(shell, 'Anchor')['fields']['rect']['x'], 99999)

    def test_only_three_virtual_cursors_change(self):
        repl = pc.build_art(self.romfs, self.out)
        self.assertEqual(set(repl), {'data/hd/global/ui/controller/cursor/' + n + '.sprite' for n in ('idle', 'hold', 'held')})
        for logical, path in repl.items():
            actual = Path(path).read_bytes()
            self.assertEqual(actual[:40], self.files[logical][:40])
            self.assertEqual(actual[40:], bytes(16))

    def test_pc_entry_packs_marker_and_only_input_resources(self):
        output = self.root / 'packed'
        generated = self.root / 'mobile-assets'
        env = dict(os.environ, DM_OVERLAY_PC='1', DM_OVERLAY_MAC='1', DM_GEN_ASSETS=str(generated))
        # Model the absent optional ITEMS slice; delegate every other import probe.
        launcher = """import importlib.util, runpy, sys
from pathlib import Path
entry = sys.argv.pop(1)
sys.argv[0] = entry
sys.path.insert(0, str(Path(entry).parent))
original_find_spec = importlib.util.find_spec
def input_only_spec(name, *args, **kwargs):
    if name == 'pc_item_layouts':
        return None
    return original_find_spec(name, *args, **kwargs)
importlib.util.find_spec = input_only_spec
runpy.run_path(entry, run_name='__main__')
"""
        result = subprocess.run([sys.executable, '-c', launcher,
                                 str(pc.HERE / 'gen_romfs_overlay.py'),
                                 str(self.romfs), str(output)],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((output / 'pc_desktop').is_file())
        self.assertFalse((output / 'ctrl_touch').exists())
        self.assertFalse(generated.exists())
        manifest = json.loads((output / 'pc_manifest.json').read_text())
        self.assertEqual(len(manifest), 13)
        self.assertFalse(Path(str(output) + '_variants').exists())
        for logical, digest in manifest.items():
            self.assertEqual(hashlib.sha256(pc.read_overlay(self.romfs, output, logical)).hexdigest(), digest)
        table=json.loads(pc.read_overlay(self.romfs,output,'data/local/lng/strings/ui.json'))
        self.assertEqual(table[0],json.loads(self.files['data/local/lng/strings/ui.json'])[0])
        rows=dm_i18n.rows()
        self.assertEqual(len(table),1+len(rows))
        for entry,(key,zh,en) in zip(table[1:],rows):
            self.assertEqual(entry['Key'],key)
            self.assertEqual(entry['zhCN'],zh)
            self.assertEqual(entry['zhTW'],zh)
            self.assertEqual(entry['enUS'],en)
            self.assertEqual(entry['esES'],en)
        # A second build must not overwrite an existing overlay.
        with patch.dict(os.environ, DM_OVERLAY_MAC='1'):
            with self.assertRaises(FileExistsError):
                pc.main(self.romfs, output)

    def test_optional_item_module_receives_prior_without_becoming_required(self):
        import types
        module = types.ModuleType('pc_item_layouts')
        seen = []
        def build(romfs, outdir, prior):
            self.assertFalse(outdir.exists())
            seen.append(set(prior))
            return {'data/item-extension': 'separate-module'}, {}
        module.build = build
        with patch.object(pc.importlib.util, 'find_spec', return_value=object()), patch.dict(sys.modules, pc_item_layouts=module):
            replacements = pc.build_replacements(self.romfs, self.out)
        self.assertEqual(len(seen[0]), 12)
        self.assertEqual(replacements['data/item-extension'], 'separate-module')

    def test_os_cursors_keep_first_frame_pixels_and_alpha(self):
        # The existing BC3 decoder is a boundary double here; the RomFS reader,
        # header checks, cropping and PNG encoding run normally.
        atlas = Image.new('RGBA', (138, 64), (18, 42, 87, 123))
        atlas.putpixel((69, 0), (255, 0, 0, 255))
        output = self.root / 'os-cursors'
        with patch.object(gen_mac_cursors.romfs_art, 'sprite', return_value=atlas):
            gen_mac_cursors.generate(self.romfs, output)
        for name in ('normal', 'pressed'):
            with Image.open(output / (name + '.png')) as image:
                self.assertEqual(image.size, (69, 64))
                self.assertEqual(image.mode, 'RGBA')
                self.assertEqual(image.tobytes(), atlas.crop((0, 0, 69, 64)).tobytes())

    def test_pc_mode_requires_mac_before_creating_output(self):
        output = self.root / 'invalid'
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(SystemExit):
                pc.main(self.romfs, output)
        self.assertFalse(output.exists())

    def test_non_pc_keeps_original_asset_generation_entry(self):
        import types
        class OriginalEntry(Exception):
            pass
        seen = []
        def ensure(romfs):
            seen.append(romfs)
            raise OriginalEntry()
        module = types.ModuleType('gen_assets')
        module.ensure = ensure
        env = {'DM_GEN_ASSETS': str(self.root / 'legacy-assets')}
        with patch.dict(os.environ, env, clear=True), patch.dict(sys.modules, gen_assets=module), patch.object(sys, 'argv', ['gen_romfs_overlay.py', str(self.romfs), str(self.out)]):
            with self.assertRaises(OriginalEntry):
                runpy.run_path(str(pc.HERE / 'gen_romfs_overlay.py'), run_name='__main__')
        self.assertEqual(seen, [str(self.romfs)])


if __name__ == '__main__':
    unittest.main()
