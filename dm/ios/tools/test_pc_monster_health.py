"""Contrato de contenedor fijo MonsterHealth PC y geometría nativa conservada."""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest

import pc_desktop_layout as pc

ROMFS = Path(os.environ.get('DM_TEST_ROMFS', str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS')))
if not ROMFS.is_dir():
    raise RuntimeError('Se requieren los recursos originales: ' + str(ROMFS))


class MonsterHealthPC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = pc.load(ROMFS, 'hudmonsterhealthhd.json')
        cls.before = copy.deepcopy(cls.original)
        with tempfile.TemporaryDirectory() as directory:
            mapping = pc.build_layouts(ROMFS, Path(directory))
            cls.page = json.loads(Path(mapping[pc.LAY + 'hudmonsterhealthhd.json']).read_text())

    def test_receiver_takes_original_screen_branch(self):
        self.assertEqual([w['name'] for w in self.page['children']], ['ScreenAnchored'],
                         'Sin ScreenAnchored, el receptor Switch mueve self hacia el objetivo')
        screen = self.page['children'][0]
        self.assertEqual(screen['type'], 'Widget')
        self.assertFalse(screen.get('fields'), 'El contenedor no debe introducir offsets ni escala')
        self.assertEqual([w['name'] for w in screen['children']], ['Health', 'Unique', 'Additional'])
        self.assertNotIn('TargetAttached', [w['name'] for w in pc.walk(self.page)])

    def test_native_health_name_and_clear_semantics_are_preserved(self):
        screen = next((w for w in pc.walk(self.page) if w.get('name') == 'ScreenAnchored'), None)
        self.assertIsNotNone(screen, 'Falta el contenedor nativo de pantalla')
        content = copy.deepcopy(screen['children'])
        health = next(w for w in content if w['name'] == 'Health')
        percentage = next(w for w in health['children'] if w['name'] == 'TouchBossPct')
        self.assertEqual(percentage['type'], 'TextBoxWidget')
        self.assertEqual(percentage['fields']['anchor'], {'x': 1, 'y': .5})
        self.assertEqual(percentage['fields']['rect'], {'x': 24, 'y': 0})
        health['children'] = [w for w in health['children'] if w['name'] != 'TouchBossPct']
        self.assertEqual(content, self.original['children'])
        self.assertEqual({k: v for k, v in self.page.items() if k != 'children'},
                         {k: v for k, v in self.original.items() if k != 'children'})

    def test_native_top_anchor_is_inside_each_ui_viewport(self):
        fields = self.page['fields']
        self.assertEqual(fields['anchor'], {'x': .5})
        self.assertEqual(fields['rect'], {'y': 63})
        for family in ('hd', 'lv', 'asian'):
            profile = pc.pc_profile(ROMFS, family)
            # Envolvente vertical: posiciones originales más una altura de letra;
            # no pretende sustituir la medición gráfica de cadenas arbitrarias.
            bottom = 63 + max(74, 85 + profile['MediumFontSize'], 130 + profile['MediumFontSize'])
            for width, height in ((1280, 720), (1280, 800), (1920, 1080), (2560, 1672),
                                  (3024, 1732), (3024, 1898), (3440, 1440), (3840, 2160)):
                for dpi in (1, 2):
                    with self.subTest(family=family, viewport=(width, height), dpi=dpi):
                        ui_height = min(height, width * 9 / 16)
                        ui_width = ui_height * 16 / 9
                        x0, y0 = (width - ui_width) / 2, height - ui_height
                        scale = ui_height / 2160
                        x, y = (x0 + ui_width / 2) * dpi, (y0 + 63 * scale) * dpi
                        self.assertGreater(x, x0 * dpi)
                        self.assertLess(x, (x0 + ui_width) * dpi)
                        self.assertGreaterEqual(y, y0 * dpi)
                        self.assertLess((y0 + bottom * scale) * dpi, height * dpi)

    def test_original_input_and_recursive_pump_contract_are_preserved(self):
        self.assertEqual(self.original, self.before)
        def depth(w):
            return 1 + max((depth(c) for c in w.get('children', [])), default=0)
        self.assertLessEqual(depth(self.page), 6, 'Health/TouchBossPct debe seguir accesible al pump existente')
        for name in ('Health', 'Name', 'Unique', 'Additional', 'TouchBossPct'):
            self.assertEqual(sum(w.get('name') == name for w in pc.walk(self.page)), 1)


if __name__ == '__main__':
    unittest.main()
