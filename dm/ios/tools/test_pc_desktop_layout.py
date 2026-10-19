"""Pruebas del overlay PC contra el RomFS original, sin arrancar el juego."""
import json
import os
from pathlib import Path
import unittest

import pc_desktop_layout as pc

ROMFS = Path(os.environ.get('DM_TEST_ROMFS', str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS')))


class DesktopLayouts(unittest.TestCase):
    def test_json_comments_do_not_damage_strings(self):
        self.assertEqual(pc.parse('{"url":"a//b",/* c */"a":[1,],}'), {'url': 'a//b', 'a': [1]})

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_native_stash_fourteen_columns_and_all_slots(self):
        original = pc.load(ROMFS, 'bankexpansionlayouthd.json')
        bank = pc.bank_layout(original)
        self.assertEqual(pc.find(bank, 'grid')['fields']['cellCount'], {'x': 14, 'y': 10})
        self.assertEqual(bank['fields']['rect']['width'], 1554)
        before = {w['fields']['itemCode'] for w in pc.walk(original) if w['type'] == 'AdvancedStashSlotWidget'}
        after = {w['fields']['itemCode'] for w in pc.walk(bank) if w['type'] == 'AdvancedStashSlotWidget'}
        self.assertEqual(before, after)
        self.assertEqual(len(after), 91)
        occupied = set()
        cells = 0
        before_fields = {w['fields']['itemCode']: w['fields'] for w in pc.walk(original) if w['type'] == 'AdvancedStashSlotWidget'}
        for slot in (w for w in pc.walk(bank) if w['type'] == 'AdvancedStashSlotWidget'):
            fields = slot['fields']
            self.assertEqual({k: v for k, v in fields.items() if k != 'rect'},
                             {k: v for k, v in before_fields[fields['itemCode']].items() if k != 'rect'})
            rect = fields['rect']
            self.assertEqual(rect['height'], before_fields[fields['itemCode']]['rect']['height'])
            self.assertEqual((rect['x'] - 89) % 98, 0)
            self.assertEqual((rect['y'] - 219) % 98, 0)
            x, y, height = (rect['x'] - 89) // 98, (rect['y'] - 219) // 98, rect['height'] // 98
            self.assertTrue(0 <= x < 14 and 0 <= y < y + height <= 10)
            these = {(x, y + d) for d in range(height)}
            self.assertFalse(occupied & these)
            occupied |= these
            cells += height
        self.assertEqual(cells, 99)
        self.assertEqual(pc.find(bank, 'BankTabs')['fields']['tabCount'], 6)
        self.assertTrue(all(s.startswith(pc.ART) for s in bank['fields']['backgroundFile']))
        self.assertNotIn('backgroundFiles', bank['fields'])
        self.assertEqual(pc.find(bank, 'close')['fields']['rect']['x'], 1467)
        self.assertIsNotNone(pc.find(bank, 'TouchVault'))
        self.assertIsNotNone(pc.find(bank, 'TouchStashLeft'))
        self.assertNotIn('Controller/', json.dumps(bank))

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_inventory_retains_native_grid_and_equipment(self):
        inv = pc.inventory_layout(pc.load(ROMFS, 'playerinventoryexpansionlayouthd.json'))
        self.assertEqual(inv['basedOn'], 'PlayerInventoryOriginalLayoutHD.json')
        self.assertIsNotNone(pc.find(inv, 'TouchPouchBox'))
        self.assertIsNotNone(pc.find(inv, 'TouchRwBox'))
        self.assertNotIn('Controller/', json.dumps(inv))

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_custom_options_keep_native_video_rows(self):
        native = pc.load(ROMFS, 'videooptionspanelhd.json')
        page = pc.custom_options(native, pc.load(ROMFS, '_profilehd.json'))
        self.assertEqual(pc.find(native, 'OptionsTable'), pc.find(page, 'OptionsTable'))
        self.assertIsNotNone(pc.find(page, 'TouchCustom_item_notes'))
        self.assertIsNotNone(pc.find(page, 'TouchCustom_lang_es'))
        self.assertNotIn('basedOn', page)

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_pause_preserves_native_entries_and_adds_working_messages(self):
        native = pc.load(ROMFS, 'pauselayoutgardenhd.json')
        page = pc.pause_layout(native)
        old = {w.get('name'): w['fields']['onClickMessage'] for w in pc.walk(native)
               if w['type'] == 'ButtonWidget' and 'onClickMessage' in w.get('fields', {})}
        for name, message in old.items():
            self.assertEqual(pc.find(page, name)['fields']['onClickMessage'], message)
        for name in ('TouchRestart', 'TouchPadDiff', 'TouchPadStore'):
            self.assertEqual(pc.find(page, name)['fields']['onClickMessage'], 'PanelManager:ClosePanel:' + name)
        self.assertIsNotNone(pc.find(page, 'TouchPadDiffTxt'))

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_pause_difficulty_uses_parent_extent_and_native_centered_typography(self):
        for relative in ('pauselayouthd.json', 'pauselayoutgardenhd.json'):
            page = pc.pause_layout(pc.load(ROMFS, relative))
            fields = pc.find(page, 'TouchPadDiffTxt')['fields']
            with self.subTest(layout=relative):
                self.assertTrue(fields.get('fitToParent'))
                self.assertNotIn('rect', fields)
            for family in ('hd', 'lv', 'asian'):
                profile = pc.pc_profile(ROMFS, family)
                def resolved(value):
                    if isinstance(value, str) and value.startswith('$'):
                        return resolved(profile[value[1:]])
                    if isinstance(value, dict):
                        return {k: resolved(v) for k, v in value.items()}
                    if isinstance(value, list):
                        return [resolved(v) for v in value]
                    return value
                style = resolved(fields['style'])
                native = resolved(pc.find(page, 'Exit')['fields']['text/style'])
                with self.subTest(layout=relative, family=family):
                    self.assertEqual(style['alignment'], {'h': 'center', 'v': 'center'})
                    for token in ('fontColor', 'pointSize', 'dropShadow', 'spacing'):
                        self.assertEqual(style[token], native[token])
            previous = Path(os.environ.get('DM_TEST_PAUSE_REFERENCE', '/nonexistent/reference'))
            if previous.is_dir():
                before = json.loads(pc.read_overlay(ROMFS, previous, pc.LAY + relative))
                pc.find(before, 'TouchPadDiffTxt')['fields'] = fields
                self.assertEqual(before, page, 'No debe cambiar ningún otro dato de la pausa congelada')

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_mercenary_ten_equipment_slots_with_native_positions(self):
        player = pc.load(ROMFS, 'playerinventoryoriginallayouthd.json')
        merc = pc.merc_layout(pc.load(ROMFS, 'hirelinginventorypanelhd.json'), player)
        slots = [w for w in pc.walk(merc) if w['type'] == 'InventorySlotWidget']
        self.assertEqual(len(slots), 10)
        for slot in slots:
            name = slot['name']
            self.assertEqual(slot['fields']['rect'], pc.find(player, name)['fields']['rect'])

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_vendor_retains_both_repair_modes(self):
        native = pc.load(ROMFS, 'vendorpanellayouthd.json')
        page = pc.vendor_layout(native)
        for child in native['children']:
            self.assertEqual(pc.find(page, child['name']), child)
        added = pc.find(page, 'TouchShopRefresh')['fields']
        self.assertEqual(added['onClickMessage'], 'PanelManager:ClosePanel:TouchShopRefresh')
        self.assertLess(added['rect']['y'] + 116 * added['rect']['scale'], 162)

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_controller_shell_preserves_class_but_has_no_visible_ornament(self):
        native = pc.load(ROMFS, 'controller/controlleroverlayhd.json')
        page = pc.controller_shell(native)
        self.assertEqual(page['type'], native['type'])
        self.assertEqual([w.get('name') for w in pc.walk(page)], [w.get('name') for w in pc.walk(native)])
        for name in ('Anchor', 'TransparentBG'):
            self.assertEqual(pc.find(page, name)['fields']['rect']['x'], 99999)
            self.assertFalse(pc.find(page, name)['fields']['fitToParent'])

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_active_profiles_use_pc_anchors_and_preserve_platform_fields(self):
        profiles = pc.desktop_profiles(ROMFS)
        self.assertEqual(len(profiles), 7)
        for rel, profile in profiles.items():
            original = pc.load(ROMFS, rel)
            target = pc.pc_profile(ROMFS, pc.profile_family(rel))
            self.assertEqual(profile['basedOn'], original['basedOn'])
            for key, value in original.items():
                if key not in target:
                    self.assertEqual(profile[key], value, (rel, key))
            for key, value in target.items():
                self.assertEqual(profile[key], value, (rel, key))
            self.assertEqual(profile['LeftPanelAnchor']['x'], 0)
            self.assertEqual(profile['RightPanelAnchor']['x'], 1)
        console = profiles['controller/_profileconsolehd.json']
        self.assertEqual(console['LeftPanelRect'], {'x': 236, 'y': -651, 'width': 1162, 'height': 1507})
        self.assertEqual(console['RightPanelRect'], {'x': -1394, 'y': -651, 'width': 1162, 'height': 1507})
        hud = pc.load(ROMFS, 'hudpanelhd.json')
        self.assertEqual(hud['fields']['anchor'], {'x': .5, 'y': 1})

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_death_text_uses_all_original_pc_languages_only(self):
        import tempfile
        import dm_i18n
        import touch_layout
        with tempfile.TemporaryDirectory() as tmp:
            result = pc.desktop_strings(ROMFS, Path(tmp))
            self.assertEqual(len(result), 1)
            logical, path = next(iter(result.items()))
            self.assertTrue(logical.endswith('/ui-controller.json'))
            _, original = touch_layout.read_layout(str(ROMFS), None, suffix='/lng/strings/ui-controller.json')
            _, ui = touch_layout.read_layout(str(ROMFS), None, suffix='/lng/strings/ui.json')
            before = {r['Key']: r for r in json.loads(original)}
            after = {r['Key']: r for r in json.loads(Path(path).read_text())}
            official = next(r for r in json.loads(ui) if r['Key'] == 'StrYouDied')
            self.assertEqual(before.keys(), after.keys())
            self.assertEqual([k for k in before if before[k] != after[k]], ['StrYouDiedConsole'])
            for lang in dm_i18n.LANGS:
                self.assertEqual(after['StrYouDiedConsole'][lang], official[lang])
                self.assertNotIn('\ue01d', after['StrYouDiedConsole'][lang])
            self.assertEqual(before['StrYouDiedConsole']['id'], after['StrYouDiedConsole']['id'])

    @unittest.skipUnless(ROMFS.is_dir(), 'RomFS local requerido')
    def test_wide_stash_retains_native_header_caps_and_bottom_frame(self):
        import romfs_art
        original = romfs_art.sprite(str(ROMFS), 'hd/global/ui/PANEL/Stash/StashPanel_BG_Personal.sprite')
        widened = pc.widen_pc_stash(original)
        self.assertEqual(widened.size, (777, 753))
        self.assertEqual(widened.crop((0, 0, 40, 753)).tobytes(), original.crop((0, 0, 40, 753)).tobytes())
        self.assertEqual(widened.crop((737, 0, 777, 753)).tobytes(), original.crop((541, 0, 581, 753)).tobytes())
        self.assertEqual(widened.crop((241, 0, 437, 108)).tobytes(), original.crop((200, 0, 396, 108)).tobytes())
        # La ampliación debe mantener metal y bordes en las últimas 31 filas;
        # la versión anterior los sustituía por piedra muestreada de y615–650.
        self.assertEqual(widened.crop((185, 722, 195, 753)).tobytes(), original.crop((185, 722, 195, 753)).tobytes())
        shared = romfs_art.sprite(str(ROMFS), 'hd/global/ui/PANEL/Stash/StashPanel_BG_Shared.sprite')
        wide_shared = pc.widen_pc_stash(shared, original)
        self.assertEqual(wide_shared.crop((283, 600, 493, 753)).tobytes(), shared.crop((185, 600, 395, 753)).tobytes())
        self.assertEqual(wide_shared.crop((185, 600, 195, 753)).tobytes(), original.crop((185, 600, 195, 753)).tobytes())

    def test_packaged_overlay(self):
        path = os.environ.get('DM_TEST_PC_OVERLAY')
        if not path:
            self.skipTest('DM_TEST_PC_OVERLAY requerido')
        pc.verify(ROMFS, Path(path))
        if (Path(path + '_variants') / 'item_notes').is_dir():
            pc.verify(ROMFS, Path(path + '_variants') / 'item_notes')
            pc.verify_variants(ROMFS, Path(path))

    def test_packaged_delta_changes_only_authorized_resources(self):
        path = os.environ.get('DM_TEST_PC_OVERLAY')
        previous = os.environ.get('DM_TEST_PC_PREVIOUS')
        if not path or not previous:
            self.skipTest('DM_TEST_PC_OVERLAY y DM_TEST_PC_PREVIOUS requeridos')
        expected = {'data/local/lng/strings/ui-controller.json'} | {
            'data/hd/global/ui/' + pc.ART.lower() + name.lower() + '.sprite'
            for name in ('StashPersonal', 'StashShared', 'StashTrove', 'VaultBottom')}
        for suffix in ('', '_variants/item_notes'):
            old = Path(previous + suffix)
            new = Path(path + suffix)
            before = json.loads((old / 'pc_manifest.json').read_text())
            after = json.loads((new / 'pc_manifest.json').read_text())
            changed = {k for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
            self.assertEqual(changed, expected)
            for logical in changed:
                import hashlib
                self.assertEqual(hashlib.sha256(pc.read_overlay(ROMFS, new, logical)).hexdigest(), after[logical])


if __name__ == '__main__':
    unittest.main()
