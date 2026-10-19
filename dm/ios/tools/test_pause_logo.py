"""Separación del título de Pausa, arte y primera fila originales de PC."""
import json
import os
from pathlib import Path
import struct
import unittest

from PIL import ImageFont
import pc_desktop_layout as pc
import romfs_art
import touch_layout

ROMFS = Path(os.environ.get('DM_TEST_ROMFS', str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS')))
if 'DM_TEST_ROMFS' in os.environ and not ROMFS.is_dir():
    raise RuntimeError('DM_TEST_ROMFS no existe: ' + str(ROMFS))


@unittest.skipUnless(ROMFS.is_dir(), 'Requiere recursos originales locales')
class PauseLogo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fonts = {name: romfs_art.font(str(ROMFS), name) for name in (
            'ExocetBlizzardOT-Medium.otf', 'IrisL.ttf', 'Formal436BT.ttf', 'BlizzardGlobal-V5_81.ttf')}
        _, raw = touch_layout.read_layout(str(ROMFS), None, suffix='/lng/strings/ui.json')
        cls.labels = next(row for row in json.loads(raw) if row['Key'] == 'Paused')
        cls.alpha = {}
        for widget in pc.find(pc.load(ROMFS, 'pauselayouthd.json'), 'LogoWidget')['children']:
            path = 'hd/global/ui/' + widget['fields']['filename'] + '.sprite'
            raw = romfs_art.read(str(ROMFS), path)
            width = struct.unpack_from('<H', raw, 6)[0]
            sheet = romfs_art.sprite(str(ROMFS), path)
            cls.alpha[widget['name']] = [sheet.crop((x, 0, x + width, sheet.height)).getchannel('A').getbbox()
                                         for x in range(0, sheet.width, width)]

    def test_title_preserves_original_geometry_and_clears_logo_and_options(self):
        for rel in ('pauselayouthd.json', 'pauselayoutgardenhd.json'):
            original = pc.load(ROMFS, rel)
            page = pc.pause_layout(original)
            title = pc.find(page, 'PauseText')
            self.assertEqual(pc.find(page, 'LogoWidget'), pc.find(original, 'LogoWidget'))
            table = pc.find(page, 'PauseTable')['fields']
            options = pc.find(page, 'Settings')['fields']
            raw = romfs_art.read(str(ROMFS), 'hd/global/ui/' + options['filename'].replace('\\', '/') + '.sprite')
            art_scale = table['columns'][0]['width'] / struct.unpack_from('<H', raw, 6)[0]
            self.assertEqual(art_scale, 2)
            logo_bottom = max(widget['fields']['rect']['y'] + art_scale * bounds[3]
                              for widget in pc.find(page, 'LogoWidget')['children']
                              for bounds in self.alpha[widget['name']] if bounds)
            first_row_top = 2160 * table['anchor']['y'] + table['rect']['y']
            margins = []
            for family in ('hd', 'lv', 'asian'):
                size = pc.pc_profile(ROMFS, family)['XExtraLargeFontSize']
                for lang in ('esES', 'esMX'):
                    for font_name, path in self.fonts.items():
                        font = ImageFont.truetype(path, size)
                        bbox = font.getbbox(self.labels[lang])
                        extent = max(abs(bbox[1]), abs(bbox[3]), *font.getmetrics())
                        y = title['fields']['rect']['y']
                        for viewport in ((1280, 720), (1920, 1080), (2560, 1440), (3024, 1700), (3024, 1900), (3840, 2160)):
                            scale = viewport[1] / 2160
                            margins.append((y - extent - logo_bottom) * scale)
                            margins.append((first_row_top - y - extent) * scale)
            self.assertGreater(min(margins), 0, 'El título invade el logo o la primera fila de opciones')
            self.assertEqual(title, pc.find(original, 'PauseText'))
            before = {w['name']: w['fields']['onClickMessage'] for w in pc.walk(original) if 'onClickMessage' in w.get('fields', {})}
            self.assertTrue(before)
            for name, message in before.items():
                self.assertEqual(pc.find(page, name)['fields']['onClickMessage'], message)
            self.assertTrue(pc.find(page, 'TouchPadDiffTxt')['fields']['fitToParent'])
            self.assertEqual(pc.find(page, 'TouchPadDiffTxt')['fields']['style'], '$StyleFEMultiLineButtonText')


if __name__ == '__main__': unittest.main()
