#!/usr/bin/env python3
"""Pixel validation and full/overlay cache isolation, without game files or generators."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

from PIL import Image

TOOLS = Path(__file__).resolve().parent.parent / 'tools'
sys.path.insert(0, str(TOOLS))
import gen_assets

# The contracts below do not read a RomFS or load the native texture conversion library.
spec = importlib.util.spec_from_file_location('native_art_generator_contract', TOOLS / 'native_art/generate.py')
native_art = importlib.util.module_from_spec(spec)
with mock.patch.dict(sys.modules, {'romfs_art': types.ModuleType('romfs_art')}):
    spec.loader.exec_module(native_art)


class PixelContract(unittest.TestCase):
    def setUp(self):
        self.image = Image.new('RGBA', (8, 4), (20, 40, 60, 128))
        self.image.putpixel((3, 1), (80, 90, 100, 255))
        self.asset = {'asset': 'test.png', 'atlas_px': list(self.image.size),
                      'decoded_rgba_sha256': hashlib.sha256(self.image.tobytes()).hexdigest(),
                      'decoded_png_sha256': 'historical-encoding-is-not-the-pixel-contract'}

    def test_different_png_encodings_with_equal_pixels_pass(self):
        encodings = []
        for level in (0, 9):
            output = io.BytesIO()
            self.image.save(output, 'PNG', compress_level=level)
            encodings.append(output.getvalue())
        self.assertNotEqual(encodings[0], encodings[1])
        for encoded in encodings:
            with Image.open(io.BytesIO(encoded)) as image:
                actual = native_art.validate_rgba(image, self.asset)
                self.assertEqual(actual.tobytes(), self.image.tobytes())

    def test_one_pixel_change_is_fatal(self):
        image = self.image.copy()
        image.putpixel((0, 0), (21, 40, 60, 128))
        with self.assertRaisesRegex(SystemExit, 'RGBA'):
            native_art.validate_rgba(image, self.asset)

    def test_same_byte_count_with_different_dimensions_is_fatal(self):
        image = Image.frombytes('RGBA', (4, 8), self.image.tobytes())
        with self.assertRaises(SystemExit):
            native_art.validate_rgba(image, self.asset)

    def test_alpha_change_is_fatal(self):
        image = self.image.copy()
        image.putpixel((0, 0), (20, 40, 60, 127))
        with self.assertRaisesRegex(SystemExit, 'RGBA'):
            native_art.validate_rgba(image, self.asset)

    def test_original_sprite_checks_still_stop_before_decode(self):
        raw = b'trusted original sprite'
        for corrupt in ('sha256', 'md5'):
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as directory:
                ios = Path(directory)
                art = ios / 'Sources/TouchArt'
                art.mkdir(parents=True)
                asset = dict(self.asset, official_path='data:data/test.sprite',
                             official_sprite_sha256=hashlib.sha256(raw).hexdigest(),
                             official_sprite_md5=hashlib.md5(raw).hexdigest())
                asset['official_sprite_' + corrupt] = '0' * (64 if corrupt == 'sha256' else 32)
                (art / 'native_art_manifest.json').write_text(
                    json.dumps({'official_tables': [], 'assets': [asset]}), encoding='utf-8')
                with mock.patch.object(native_art, 'IOS', ios), \
                     mock.patch.object(native_art.romfs_art, 'read', return_value=raw, create=True), \
                     mock.patch.object(native_art.romfs_art, 'sprite', create=True) as decode:
                    with self.assertRaises(SystemExit):
                        native_art.main('unused')
                    decode.assert_not_called()
                self.assertFalse((art / asset['asset']).exists())


class GeneratorSelection(unittest.TestCase):
    def run_commands(self, overlay_only=False):
        completed = types.SimpleNamespace(returncode=0, stdout='', stderr='')
        with mock.patch.object(gen_assets.subprocess, 'run', return_value=completed) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            gen_assets.run_steps('input-romfs', overlay_only=overlay_only)
        return [call.args[0] for call in run.call_args_list]

    def test_full_is_default_and_keeps_skill_generator(self):
        commands = self.run_commands()
        self.assertTrue(any(any(Path(arg).as_posix().endswith('/native_art/generate.py') for arg in command) for command in commands))
        self.assertFalse(any('--overlay-only' in command for command in commands))

    def test_overlay_skips_skill_generator_and_passes_flag_only_to_hud(self):
        commands = self.run_commands(overlay_only=True)
        self.assertFalse(any(any(Path(arg).as_posix().endswith('/native_art/generate.py') for arg in command) for command in commands))
        flagged = [command for command in commands if '--overlay-only' in command]
        self.assertEqual(len(flagged), 1)
        self.assertTrue(any(Path(arg).name == 'gen_hud_v4.py' for arg in flagged[0]))
        full = self.run_commands()
        self.assertEqual(len(full), len(commands) + 1)

    def test_overlay_does_not_turn_required_asset_failure_into_warning(self):
        fail = types.SimpleNamespace(returncode=1, stdout='', stderr='required asset missing')
        with mock.patch.object(gen_assets.subprocess, 'run', return_value=fail) as run:
            with self.assertRaisesRegex(SystemExit, 'required asset missing'):
                gen_assets.run_steps('input-romfs', overlay_only=True)
        self.assertEqual(run.call_count, 1)

    def test_scope_selection_does_not_change_process_environment(self):
        before = dict(os.environ)
        self.run_commands(overlay_only=True)
        self.assertEqual(dict(os.environ), before)


class ModeCache(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.tools = self.root / 'ios/tools'
        self.tools.mkdir(parents=True)
        (self.tools / 'generator.py').write_text('# synthetic generator source\n', encoding='utf-8')
        self.romfs = self.root / 'romfs'
        (self.romfs / 'data').mkdir(parents=True)
        (self.romfs / 'data/build_manifest.psv').write_bytes(b'frozen-test-input\n')
        self.output = self.root / 'output'
        self.output.mkdir()
        self.patchers = [mock.patch.object(gen_assets, 'HERE', str(self.tools)),
                         mock.patch.object(gen_assets.gen_paths, 'gen_dir', return_value=str(self.output))]
        for patcher in self.patchers:
            patcher.start()
            self.addCleanup(patcher.stop)
        self.calls = []

    def fake_generate(self, romfs, overlay_only=False):
        self.calls.append(overlay_only)
        v4 = self.output / 'NativeHUD/v4'
        files = [v4 / 'base.png', v4 / 'hud.json', v4 / 'hud_v4_ui/ui.json']
        silhouette = v4 / 'hud_v4_ui/sil_dm_native_skill_0.png'
        if overlay_only:
            # The HUD generator clears this directory before filling the requested output set.
            silhouette.unlink(missing_ok=True)
        else:
            files += [self.tools.parent / 'Sources/TouchArt/dm_native_skill_0.png',
                      self.tools.parent / 'Sources/dm_touch_native_art.c', silhouette]
        for path in files:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic output')

    def test_overlay_can_be_fresh_without_any_skill_output(self):
        with mock.patch.object(gen_assets, 'run_steps', side_effect=self.fake_generate):
            self.assertTrue(gen_assets.ensure(str(self.romfs), overlay_only=True))
            self.assertFalse((self.tools.parent / 'Sources/TouchArt/dm_native_skill_0.png').exists())
            self.assertFalse(gen_assets.ensure(str(self.romfs), overlay_only=True))
        self.assertEqual(self.calls, [True])

    def test_overlay_reuses_full_cache_and_preserves_skill_images(self):
        with mock.patch.object(gen_assets, 'run_steps', side_effect=self.fake_generate):
            self.assertTrue(gen_assets.ensure(str(self.romfs)))
            self.assertFalse(gen_assets.ensure(str(self.romfs)))
            full_stamp = (self.output / '.stamp').read_text()
            images = [self.tools.parent / 'Sources/TouchArt/dm_native_skill_0.png',
                      self.output / 'NativeHUD/v4/hud_v4_ui/sil_dm_native_skill_0.png']
            # 用不同内容检测缓存复用时发生的删除或重写。
            # Distinct contents detect deletion or rewriting during cache reuse.
            for i, path in enumerate(images):
                path.write_bytes(f'preserved skill image {i}'.encode())
            expected = [path.read_bytes() for path in images]
            self.assertFalse(gen_assets.ensure(str(self.romfs), overlay_only=True))
            self.assertFalse(gen_assets.ensure(str(self.romfs), overlay_only=True))
            self.assertEqual((self.output / '.stamp').read_text(), full_stamp)
            self.assertEqual([path.read_bytes() for path in images], expected)
            self.assertFalse(gen_assets.ensure(str(self.romfs)))
        self.assertEqual(self.calls, [False])

    def test_full_request_upgrades_overlay_cache_then_reuses_it(self):
        with mock.patch.object(gen_assets, 'run_steps', side_effect=self.fake_generate):
            self.assertTrue(gen_assets.ensure(str(self.romfs), overlay_only=True))
            overlay_stamp = (self.output / '.stamp').read_text()
            self.assertFalse(gen_assets.ensure(str(self.romfs), overlay_only=True))
            self.assertTrue(gen_assets.ensure(str(self.romfs)))
            self.assertNotEqual((self.output / '.stamp').read_text(), overlay_stamp)
            self.assertTrue((self.tools.parent / 'Sources/TouchArt/dm_native_skill_0.png').is_file())
            self.assertTrue((self.output / 'NativeHUD/v4/hud_v4_ui/sil_dm_native_skill_0.png').is_file())
            self.assertFalse(gen_assets.ensure(str(self.romfs)))
        self.assertEqual(self.calls, [True, False])

    def test_missing_full_silhouette_invalidates_full_cache(self):
        with mock.patch.object(gen_assets, 'run_steps', side_effect=self.fake_generate):
            self.assertTrue(gen_assets.ensure(str(self.romfs)))
            (self.output / 'NativeHUD/v4/hud_v4_ui/sil_dm_native_skill_0.png').unlink()
            self.assertTrue(gen_assets.ensure(str(self.romfs)))
        self.assertEqual(self.calls, [False, False])

    def test_failed_generation_never_marks_cache_fresh_and_releases_lock(self):
        with mock.patch.object(gen_assets, 'run_steps', side_effect=SystemExit('broken source')):
            with self.assertRaisesRegex(SystemExit, 'broken source'):
                gen_assets.ensure(str(self.romfs), overlay_only=True)
        self.assertFalse((self.output / '.stamp').exists())
        self.assertFalse((self.output / '.gen_lock').exists())
        with mock.patch.object(gen_assets, 'run_steps', side_effect=self.fake_generate):
            self.assertTrue(gen_assets.ensure(str(self.romfs), overlay_only=True))


if __name__ == '__main__':
    unittest.main()
