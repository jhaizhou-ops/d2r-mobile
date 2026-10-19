"""Overlay Mac con layouts PC originales y las funciones del port.

Los nombres Touch* son el protocolo de los módulos C existentes, no controles
táctiles. Este módulo no genera HUD móvil ni cambia perfiles tipográficos.
"""
import copy
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import touch_layout

HERE = Path(__file__).resolve().parent
LAY = 'data/global/ui/layouts/'
ART = 'PANEL/Zdesktop_pc/'
EX = 392
CONTROLLER_PROFILES = ('hd', 'lv', 'asian', 'consolehd', 'consolelv', 'handheldhd', 'handheldlv')


def parse(text):
    """JSON original con comentarios y comas finales; conserva cadenas literalmente."""
    out = []
    i = 0
    quoted = False
    while i < len(text):
        c = text[i]
        if quoted:
            out.append(c)
            if c == '\\':
                i += 1
                out.append(text[i])
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
            out.append(c)
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = len(text) if j < 0 else j
            continue
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            if j < 0:
                raise ValueError('Comentario sin cerrar')
            out.append(' ')
            i = j + 2
            continue
        else:
            out.append(c)
        i += 1
    text = ''.join(out)
    out = []
    i = 0
    quoted = False
    while i < len(text):
        c = text[i]
        if quoted:
            out.append(c)
            if c == '\\':
                i += 1
                out.append(text[i])
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
            out.append(c)
        elif c == ',' and re.match(r'\s*[}\]]', text[i + 1:]):
            pass
        else:
            out.append(c)
        i += 1
    return json.loads(''.join(out).lstrip('\ufeff'))


def load(romfs, rel):
    return parse(touch_layout.read_layout(str(romfs), rel)[1])


def walk(widget):
    yield widget
    for child in widget.get('children', []):
        yield from walk(child)


def find(widget, name):
    matches = [w for w in walk(widget) if w.get('name') == name]
    if len(matches) != 1:
        raise ValueError('%s: se esperaba un widget, hay %d' % (name, len(matches)))
    return matches[0]


def fragment(text):
    return parse('{"children":[' + text + ']}')['children']


def clean_selection(widget):
    """Los bordes de selección añadidos usan el marco de tooltip PC original."""
    for w in walk(widget):
        fields = w.get('fields', {})
        filename = fields.get('filename', '')
        if filename.lower() == 'controller/panel/controllerselector/controllerselectorborder':
            fields['filename'] = 'PANEL/TooltipBorder/TooltipBorder'
    return widget


def text(name, value, x, y, width, height=64):
    return {'type': 'TextBoxWidget', 'name': name, 'fields': {
        'rect': {'x': x, 'y': y, 'width': width, 'height': height}, 'text': value,
        'style': {'fontColor': '$FontColorGoldYellow', 'pointSize': '$MediumFontSize',
                  'alignment': {'h': 'center', 'v': 'center'}}}}


def page_button(name, x, y, direction):
    return {'type': 'ButtonWidget', 'name': name, 'fields': {
        'rect': {'x': x, 'y': y, 'width': 64, 'height': 64},
        'filename': 'PANEL/Stash/Stash_%sArrow' % direction,
        'normalFrame': 0, 'hoveredFrame': 1, 'pressedFrame': 2, 'disabledFrame': 3,
        'onClickMessage': 'PanelManager:ClosePanel:TouchPersonalStash' + ('Previous' if direction == 'Left' else 'Next')}}


def vault_layout(original):
    """Cámara PC dentro del área nativa; nombres conservados para dm_vault_ui.c."""
    vault = clean_selection(copy.deepcopy(original))
    vault['fields']['rect'].update(width=1554, height=1507)
    find(vault, 'TouchV_Back')['fields']['rect'] = {'x': 82, 'y': 219, 'width': 1390, 'height': 981}
    find(vault, 'TouchV_Cover')['fields'].update(rect={'x': 0, 'y': 1200}, filename=ART + 'VaultBottom')
    find(vault, 'TouchV_Top')['fields']['rect'] = {'x': 105, 'y': 235, 'width': 1340, 'height': 78}
    for name in ('TouchV_SubU', 'TouchV_SubS'):
        find(vault, name)['fields']['rect']['y'] = 6
    find(vault, 'TouchV_SearchBg')['fields']['rect'].update(x=524, y=0, width=1088)
    find(vault, 'TouchV_Search')['fields']['rect']['width'] = 1016
    find(vault, 'TouchV_Parts')['fields']['rect'] = {'x': 105, 'y': 325, 'width': 1340, 'height': 168}
    for i in range(11):
        part = find(vault, 'TouchV_PT%d' % i)
        part['fields']['rect'] = {'x': (i % 6) * 224 + (115 if i >= 6 else 0), 'y': (i // 6) * 88, 'width': 214, 'height': 80}
        hit = find(part, 'Hit')
        hit['fields']['rect'].update(width=214, height=80)
        normal = copy.deepcopy(hit)
        normal['name'] = 'Bg'
        normal['fields'].update(filename='PANEL/TooltipBorder/TooltipBorder', tileScale=0.5, backgroundColor=[0.08, 0.075, 0.07, 1.0])
        active = copy.deepcopy(normal)
        active['name'] = 'BgActive'
        active['fields']['backgroundColor'] = [0.19, 0.15, 0.07, 1.0]
        labels = [find(part, name) for name in ('Text', 'TextOn')]
        for label in labels:
            label['fields']['rect'] = {'x': 8, 'y': 8, 'width': 198, 'height': 64}
            label['fields']['style'].update(pointSize='$SmallFontSize', spacing='$MinimumSpacing',
                                            options={'newlineHandling': 'standard', 'lineWrap': False})
        part['children'] = [normal, active, hit, *labels]
    find(vault, 'TouchV_Stats')['fields']['rect'] = {'x': 105, 'y': 505, 'width': 1340, 'height': 40}
    find(vault, 'TouchV_Tiers')['fields']['rect'] = {'x': 105, 'y': 557, 'width': 1340, 'height': 65}
    for i, key in enumerate(('NormalQuality', 'ExceptionalQuality', 'EliteQuality')):
        find(vault, 'TouchV_T%d' % i)['fields']['rect']['x'] = i * 440
        label = find(vault, 'TouchV_T%d_Text' % i)['fields']
        label.update(text='@' + key, fitToText=False, truncateWithEllipsis=False,
                     rect={'x': i * 440 + 75, 'y': 0, 'width': 340, 'height': 65},
                     style={'fontColor': '$FontColorGoldYellow', 'pointSize': '$MediumFontSize', 'alignment': {'h': 'left', 'v': 'center'}})
    find(vault, 'TouchV_Uniq')['fields']['rect'] = {'x': 113, 'y': 638, 'width': 1324, 'height': 544}
    for i in range(12):
        cell = find(vault, 'TouchV_U%d' % i)
        cell['fields']['rect'] = {'x': (i % 3) * 444, 'y': (i // 3) * 138, 'width': 436, 'height': 130}
        find(cell, 'Bg')['fields']['rect']['width'] = 436
        find(cell, 'CellGold')['fields']['rect']['width'] = 472
        find(cell, 'Name')['fields']['rect'].update(y=8, width=276, height=64)
        find(cell, 'Name')['fields']['style']['options'] = {'newlineHandling': 'standard', 'lineWrap': True}
        find(cell, 'Sub')['fields']['rect'].update(y=80, width=312, height=40)
        find(cell, 'Dot')['fields']['rect']['x'] = 382
    find(vault, 'TouchV_Sets')['fields']['rect'] = {'x': 99999, 'y': 377, 'width': 1340, 'height': 804}
    for i in range(7):
        row = find(vault, 'TouchV_S%d' % i)
        row['fields']['rect'] = {'x': 0, 'y': i * 116, 'width': 1340, 'height': 108}
        for name in ('Bg', 'BgGold'):
            find(row, name)['fields']['rect'].update(width=1340, height=108)
        find(row, 'Name')['fields']['rect'] = {'x': 16, 'y': 4, 'width': 500, 'height': 66}
        find(row, 'Name')['fields']['style'].update(pointSize='$MediumFontSize', options={'newlineHandling': 'standard', 'lineWrap': True})
        find(row, 'Count')['fields']['rect'] = {'x': 16, 'y': 74, 'width': 500, 'height': 30}
        for j in range(6):
            x = 540 + j * 132
            find(row, 'Tile%d' % j)['fields']['rect'] = {'x': x, 'y': 6, 'width': 124, 'height': 96}
            find(row, 'Tile%dGold' % j)['fields']['rect'] = {'x': x - 18, 'y': -12, 'width': 160, 'height': 132}
            find(row, 'Tile%dBox' % j)['fields']['rect'] = {'x': x + 8, 'y': 14, 'width': 108, 'height': 80}
            find(row, 'Tile%dDot' % j)['fields']['rect']['x'] = x + 80
    for name, x in (('TouchV_PgL', 503), ('TouchV_PgR', 934)):
        find(vault, name)['fields']['rect'].update(x=x, y=1252)
    for name, x in (('TouchV_PgLHit', 483), ('TouchV_PgRHit', 913)):
        find(vault, name)['fields']['rect'] = {'x': x, 'y': 1240, 'width': 160, 'height': 160}
    page=find(vault, 'TouchV_Page')['fields']
    page['rect'].update(x=637, y=1280)
    page['truncateWithEllipsis']=False
    page['style'].update(pointSize='$MediumLargeFontSize',spacing='$MinimumSpacing',
                         options={'lineWrap':False,'newlineHandling':'standard'})
    return vault


def bank_layout(original):
    import wide_stash
    bank = copy.deepcopy(original)
    fields = bank.setdefault('fields', {})
    fields['rect'] = {'x': 40, 'y': -651, 'width': 1554, 'height': 1507}
    grid = find(bank, 'grid')['fields']
    assert grid['cellCount'] == {'x': 10, 'y': 10}, grid
    assert grid['rect'] == {'x': 89, 'y': 219}, grid
    grid['cellCount']['x'] = 14
    for name, dx in [('gold_amount', 196), ('gold_withdraw', 196), ('StashNavigation', 196), ('PreviousSeasonToggleDisplay', 392)]:
        find(bank, name)['fields']['rect']['x'] += dx
    assert len(fields['backgroundFile']) == 5
    fields['backgroundFile'] = [ART + 'StashPersonal', ART + 'StashShared', ART + 'StashTrove', ART + 'StashTrove', ART + 'StashTrove']
    find(bank, 'background')['fields']['filename'] = ART + 'StashPersonal'
    # Conserva los 91 códigos y sus alturas originales; caben en la cuadrícula 14×10.
    groups = [find(bank, n) for n in ('advancedstash_gems', 'advancedstash_materials', 'advancedstash_runes')]
    slots = [w for group in groups for w in group.get('children', []) if w['type'] == 'AdvancedStashSlotWidget']
    assert len(slots) == 91 and len({w['fields']['itemCode'] for w in slots}) == 91
    used = set()
    for slot in slots:
        r = slot['fields']['rect']
        height = 2 if r['height'] > r['width'] else 1
        for y in range(10 - height + 1):
            for x in range(14):
                cells = {(x, y + dy) for dy in range(height)}
                if not cells & used:
                    used |= cells
                    r.update(x=89 + 98 * x, y=219 + 98 * y, width=98, height=98 * height)
                    break
            else:
                continue
            break
        else:
            raise ValueError('Los objetos del depósito no caben en 14×10')
    for group in groups:
        group['children'] = [w for w in group.get('children', []) if w['type'] != 'AdvancedStashSlotWidget']
    groups[0].setdefault('children', []).extend(slots)
    tabs = find(bank, 'BankTabs')['fields']
    assert tabs['tabCount'] == 5
    tabs.update(tabCount=6, inactiveFrames=[0] * 6, activeFrames=[1] * 6,
                disabledFrames=[-1, 0, 0, 0, 0, -1],
                textStrings=['@personal', '@shared', '@dmTroveTab', '@materials', '@runes', '@dmStashVaultTab'])
    vault = vault_layout(wide_stash.vault_widgets())
    page = {'type': 'Widget', 'name': 'TouchPgPage', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1554, 'height': 1507}},
            'children': [text('TouchPgPage_Text', '', 565, 1215, 420)]}
    find(bank, 'close')['fields'] = {'rect': {'x': 1467, 'y': 9}}
    find(bank, 'click_catcher')['fields'] = {'rect': {'x': 0, 'y': 0, 'width': 1554, 'height': 1507}}
    find(bank, 'title')['fields'] = {'rect': {'x': 287, 'y': 64, 'width': 972, 'height': 71}}
    page['children'] += [page_button('TouchStashLeft', 565, 1215, 'Left'), page_button('TouchStashRight', 921, 1215, 'Right')]
    bank['children'] += [page, vault]
    return bank


def inventory_layout(original):
    import pouch_layout
    inv = copy.deepcopy(original)
    inv['children'] += fragment(touch_layout._RW) + fragment(pouch_layout.widgets())
    clean_selection(inv)
    pouch = find(inv, 'TouchPouch')['fields']
    pouch.update(filename=ART + 'PouchBox')
    pouch['rect'] = {'x': 90, 'y': 1344, 'width': 280, 'height': 100}
    find(inv, 'TouchPouchIcon')['fields']['rect'] = {'x': 106, 'y': 1352, 'scale': 0.8}
    find(inv, 'TouchPouchCount')['fields']['rect'].update(x=204, y=1344, height=100)
    find(inv, 'TouchStore')['fields']['rect'] = {'x': 800, 'y': 1348, 'width': 366, 'scale': 0.65}
    return inv


def pause_layout(original):
    pause = copy.deepcopy(original)
    table = find(pause, 'PauseTable')
    # Un solo listado PC; conserva también las filas nativas de Crónica/filtro.
    for child in list(pause['children']):
        if child['type'] == 'TableWidget' and child is not table:
            table['children'].extend(child.get('children', []))
            pause['children'].remove(child)
    pause['children'] = [w for w in pause['children'] if w.get('name') != 'LootChron Separator']
    row = next(w for w in table['children'] if any(c.get('name') == 'Exit' for c in walk(w)))
    for name, label in [('TouchRestart', '@dmPauseRestart'), ('TouchPadDiff', ''), ('TouchPadStore', '@strGoldDeposit')]:
        new = copy.deepcopy(row)
        new['name'] = 'Row ' + name
        btn = find(new, 'Exit')
        btn['name'] = name
        btn['fields'].update(textString=label, onClickMessage='PanelManager:ClosePanel:' + name)
        if name == 'TouchPadDiff':
            btn['children'] = [{'type': 'TextBoxWidget', 'name': 'TouchPadDiffTxt', 'fields': {
                'fitToParent': True, 'text': '@OptionsMenuGameDifficulty',
                'style': '$StyleFEMultiLineButtonText'}}]
        table['children'].append(new)
    table['fields']['rect']['y'] = -380
    return pause


def vendor_layout(original):
    vendor = copy.deepcopy(original)
    refresh = copy.deepcopy(find(vendor, 'button_refresh'))
    refresh['name'] = 'TouchShopRefresh'
    # El hueco central del pie contiene el oro; la acción añadida ocupa el margen
    # izquierdo del título, conservando ambos modos originales de reparación.
    refresh['fields']['rect'] = {'x': 120, 'y': 72, 'scale': 0.55}
    refresh['fields']['onClickMessage'] = 'PanelManager:ClosePanel:TouchShopRefresh'
    vendor['children'].append(refresh)
    return vendor


def character_layout(original):
    page = copy.deepcopy(original)
    # Flujo PC: el nombre y la dificultad se eligen en la misma pantalla.
    page['children'] += fragment(touch_layout._BLISS)
    find(page, 'TouchBliss')['fields']['rect'].update(x=-1680, y=1740)
    find(page, 'TouchBlissTitle')['fields']['rect'].update(x=-1580, y=1732, width=1000)
    find(page, 'TouchBlissDesc')['fields']['rect'].update(x=-1680, y=1810, width=1120, height=100)
    # La política PC conserva el enlace nativo sólo para su clase admitida.
    find(page, 'UpsellDescription')['fields']['rect'].update(x=-1420, y=-428, width=450, height=180)
    find(page, 'UpsellButton')['fields']['rect'].update(x=-1420)
    return page


def monster_health(original):
    page = copy.deepcopy(original)
    health = find(page, 'Health')
    health.setdefault('children', []).append({'type': 'TextBoxWidget', 'name': 'TouchBossPct', 'fields': {
        'anchor': {'x': 1, 'y': 0.5}, 'rect': {'x': 24, 'y': 0},
        'style': {'pointSize': '$MediumFontSize', 'fontColor': '$FontColorGoldYellow', 'alignment': {'h': 'left', 'v': 'center'}}}})
    # El receptor Switch trata self como TargetAttached si falta ScreenAnchored.
    page['children'] = [{'type': 'Widget', 'name': 'ScreenAnchored', 'children': page['children']}]
    return page


def controller_shell(original):
    """La clase sigue disponible; los paneles PC ya dibujan sus propios marcos."""
    shell = copy.deepcopy(original)
    for name in ('TransparentBG', 'Anchor'):
        fields = find(shell, name).setdefault('fields', {})
        fields.update(fitToParent=False, fitToScreen=False,
                      rect={'x': 99999, 'y': 0, 'width': 2880, 'height': 1763})
    return shell


def profile_family(relative):
    name = Path(relative).stem
    return 'asian' if name.endswith('asian') else 'lv' if name.endswith('lv') else 'hd'


def pc_profile(romfs, family):
    """Tokens PC resueltos, conservando sus variantes de idioma y accesibilidad."""
    values = load(romfs, '_profilehd.json')
    if family != 'hd':
        child = load(romfs, '_profile' + family + '.json')
        assert child.pop('basedOn') == 'HD'
        values.update(child)
    assert 'basedOn' not in values
    return values


def desktop_profiles(romfs):
    profiles = {}
    for suffix in CONTROLLER_PROFILES:
        relative = 'controller/_profile' + suffix + '.json'
        values = load(romfs, relative)
        assert values.get('basedOn')
        # El motor conserva su familia de plataforma; las variables compartidas
        # usadas por los layouts PC deben resolver a los valores PC originales.
        values.update(copy.deepcopy(pc_profile(romfs, profile_family(relative))))
        profiles[relative] = values
    return profiles


def desktop_strings(romfs, outdir):
    import dm_i18n
    logical, original = touch_layout.read_layout(str(romfs), None, suffix='/lng/strings/ui-controller.json')
    _, source = touch_layout.read_layout(str(romfs), None, suffix='/lng/strings/ui.json')
    official = next(r for r in json.loads(source) if r['Key'] == 'StrYouDied')
    key = re.search(r'"Key"\s*:\s*"StrYouDiedConsole"', original)
    assert key
    start = original.rfind('{', 0, key.start())
    end = touch_layout._block_end(original, start) + 1
    block = original[start:end]
    assert json.loads(block)['Key'] == 'StrYouDiedConsole'
    for lang in dm_i18n.LANGS:
        pattern = r'("' + re.escape(lang) + r'"\s*:\s*)("(?:[^"\\]|\\.)*")'
        block, count = re.subn(pattern, lambda m: m[1] + json.dumps(official[lang], ensure_ascii=False), block)
        assert count == 1, lang
    output = outdir / 'pc_ui-controller.json'
    output.write_text(original[:start] + block + original[end:], encoding='utf-8')
    return {logical.lower(): str(output)}


def tile_horizontal(destination, source, x, y, width):
    from PIL import Image
    offset = 0
    while offset < width:
        tile = source if (offset // source.width) % 2 == 0 else source.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        part = tile.crop((0, 0, min(tile.width, width - offset), tile.height))
        destination.paste(part, (x + offset, y))
        offset += part.width


def widen_pc_stash(source, footer_source=None):
    from PIL import Image
    assert source.size == (581, 753)
    output = Image.new('RGBA', (777, 753))
    # Los adornos del título no son columnas de inventario. Extender solo piedra
    # central aquí evita repetir el remate izquierdo con la primera columna.
    output.paste(source.crop((0, 0, 241, 108)), (0, 0))
    output.paste(source.crop((200, 0, 396, 108)), (241, 0))
    output.paste(source.crop((241, 0, 581, 108)), (437, 0))
    # La cuadrícula sí necesita cuatro columnas originales de 49 píxeles.
    output.paste(source.crop((0, 108, 241, 600)), (0, 108))
    output.paste(source.crop((45, 108, 241, 600)), (241, 108))
    output.paste(source.crop((241, 108, 581, 600)), (437, 108))
    output.paste(source.crop((0, 600, 185, 753)), (0, 600))
    output.paste(source.crop((185, 600, 395, 753)), (283, 600))
    output.paste(source.crop((395, 600, 581, 753)), (591, 600))
    # Esta tira incluye también el borde inferior metálico en sus filas originales.
    # El compartido ocupa esta zona con la placa de navegación; la piedra libre
    # procede del personal, que comparte el mismo marco original.
    strip = (footer_source or source).crop((185, 600, 195, 753))
    tile_horizontal(output, strip, 185, 600, 98)
    tile_horizontal(output, strip, 493, 600, 98)
    return output


def vault_bottom(source, widened):
    bottom = widened.crop((0, 600, 777, 753))
    # Solo se elimina el campo de oro y su sombra; se conservan piedra, adornos y
    # la banda inferior de la ampliación. La muestra procede del mismo nivel y.
    strip = source.crop((185, 656, 195, 720))
    tile_horizontal(bottom, strip, 288, 56, 206)
    return bottom


def merc_layout(original, player):
    import merc_slots
    merc = copy.deepcopy(original)
    slots = {s: copy.deepcopy(find(player, 'slot_' + s)) for s in merc_slots.ALL}
    for name, slot in slots.items():
        f = slot['fields']
        f['isHireable'] = True
        if name in merc_slots.NEW:
            merc['children'].append(slot)
            r = dict(f['rect'], x=99999)
            merc['children'].append({'type': 'RectangleWidget', 'name': 'TouchMercLock_' + name,
                                     'fields': {'rect': r, 'color': [0.187, 0.019, 0.022, 0.78]}})
        else:
            find(merc, 'slot_' + name)['fields']['rect'] = f['rect']
    find(merc, 'Background')['fields']['filename'] = ART + 'HirelingPanel'
    find(merc, 'CharacterName')['fields']['rect']['y'] = 882
    find(merc, 'HireTypeText')['fields']['rect'].update(x=700, y=882, width=320, height=50)
    find(merc, 'XpProgress')['fields']['rect']['y'] = 935
    for name in ('Skill0', 'Skill1', 'Skill2'):
        find(merc, name)['fields']['rect']['y'] = 974
    return merc


def custom_options(original, profile):
    page = copy.deepcopy(original)
    custom = parse(touch_layout._custom_page(json.dumps(original))[0])
    rows = clean_selection(find(custom, 'TouchCustomRows'))
    table = find(page, 'OptionsTable')
    def number(value):
        return number(profile[value[1:]]) if isinstance(value, str) and value.startswith('$') else value
    height = sum(number(row.get('fields', {}).get('rowHeight', table['fields']['rowHeight'])) for row in table['children'])
    rows['fields']['rect']['y'] = height + 180
    rows['children'].insert(0, text('TouchCustomTitle', '@KeyBindingTypeCustom', 0, 0, 1500, 180))
    find(page, 'ScrollView')['children'].append(rows)
    return page


def rift_layout(romfs):
    page = parse(touch_layout._rift_panel(touch_layout.read_layout(str(romfs), 'controller/messagelogpanelhd.json')[1])[0])
    removed = {'TouchRF_CloseX', 'TouchClose'}
    for w in walk(page):
        if 'children' in w:
            w['children'] = [c for c in w['children'] if c.get('name') not in removed and not c.get('name', '').startswith(('TouchRF_KeyA', 'TouchRF_KeyX'))]
    return clean_selection(page)


def chronicle_layout(romfs):
    import romfs_art
    page=clean_selection(parse(touch_layout._chronicle_killboard(touch_layout.read_layout(str(romfs),'chroniclepanelhd.json')[1])[0]))
    tabs=find(page,'ChronicleTabs')['fields']
    profile=pc_profile(romfs,'hd')
    raw=romfs_art.read(str(romfs),'hd/global/ui/'+tabs['filename'].replace('\\','/')+'.sprite')
    assert raw[:4]==b'SpA1' and struct.unpack_from('<H',raw,4)[0]==61
    tabs['tabSize']['x']=2*struct.unpack_from('<H',raw,6)[0]
    tabs['rect']=dict(profile['SettingsPanelTabsRect'],width=profile['ChronicleSearchBox']['x'])
    tabs['textStyle']['fontFace']='Exocet'
    tabs['textStyle']['pointSize']='$SmallPanelFontSize'
    tabs['textStyle']['spacing']['kerning']=profile['MinimumSpacing']['kerning']
    tabs['textStyle']['options']={'lineWrap':False,'newlineHandling':'standard'}
    tabs['truncateText']=False
    return page


def build_data(romfs, outdir):
    """Solo tablas compartidas; no llama wide_stash.build, que cambia arte Controller."""
    import wide_stash
    result = {}
    for sub in ('', 'base/'):
        logical, raw = wide_stash._entry(str(romfs), '/global/excel/' + sub + 'inventory.bin')
        inv = bytearray(raw)
        for row in (10, 25):
            offset = 4 + row * 240 + 16
            assert inv[offset:offset + 2] == b'\x0a\x0a'
            inv[offset] = 14
        for row, dx, dy in ((11, -320, 0), (26, -240, 60)):
            base = 4 + row * 240 + 0x28
            assert struct.unpack_from('<4i', inv, base + 4 * 20) == (-1,) * 4
            for k in range(4, 10):
                l, r, t, b, x = struct.unpack_from('<5i', inv, 4 + 0x28 + k * 20)
                struct.pack_into('<5i', inv, base + k * 20, l + dx, r + dx, t + dy, b + dy, x)
        p = outdir / ('inventory_' + sub.replace('/', '_') + '.bin')
        p.write_bytes(inv)
        result[logical] = str(p)
    return result


def build_cursor_art(romfs, outdir):
    """Solo los tres aros Controller; el guante PC y el objeto sostenido son otros recursos."""
    return touch_layout.build_cursor(str(romfs), str(outdir))


def build_art(romfs, outdir):
    import romfs_art
    import gen_widestash_art as art
    from PIL import Image
    library = art.lib()
    result = {}
    paths = {'StashPersonal': 'PANEL/Stash/StashPanel_BG_Personal', 'StashShared': 'PANEL/Stash/StashPanel_BG_Shared',
             'HirelingPanel': 'PANEL/Hireling/HirelingPanel', 'Inventory': 'PANEL/Inventory/Background'}
    raws = {name: romfs_art.read(str(romfs), 'hd/global/ui/' + rel + '.sprite') for name, rel in paths.items()}
    images = {name: art.decode(library, raw) for name, raw in raws.items()}
    assert all(im.size == (581, 753) for im in images.values())
    def save(name, image, header):
        path = outdir / (name + '.sprite')
        path.write_bytes(art.pack(library, header, art.bc3(image), image.width, image.height, image.width))
        result['data/hd/global/ui/' + ART.lower() + name.lower() + '.sprite'] = str(path)
        image.save(outdir / (name + '.png'))
    for name in ('StashPersonal', 'StashShared'):
        src = images[name]
        im = widen_pc_stash(src, images['StashPersonal'])
        save(name, im, raws[name])
        if name == 'StashPersonal':
            save('StashTrove', im, raws[name])
            save('VaultBottom', vault_bottom(src, im), raws[name])
    merc = images['HirelingPanel'].copy()
    merc.paste(images['Inventory'].crop((35, 80, 550, 440)), (35, 80))
    merc.paste(images['HirelingPanel'].crop((35, 405, 550, 445)), (35, 440))
    save('HirelingPanel', merc, raws['HirelingPanel'])
    pouch = images['Inventory'].crop((195, 663, 391, 715)).resize((140, 50), Image.Resampling.LANCZOS)
    save('PouchBox', pouch, raws['Inventory'])
    result.update(build_cursor_art(romfs, outdir))
    return result


def character_select_layout(original):
    """El handler Switch exige el argumento offline que ya añade touch_layout._pc_charselect."""
    page = copy.deepcopy(original)
    button = find(page, 'CreateCharacter')['fields']
    assert button['onClickMessage'] == 'FrontEndNavigation:ToCharacterCreate'
    button['onClickMessage'] += ':1'
    return page


def build_layouts(romfs, outdir):
    result = {}
    changes = {
        'characterselectpanelhd.json': character_select_layout(load(romfs, 'characterselectpanelhd.json')),
        'bankexpansionlayouthd.json': bank_layout(load(romfs, 'bankexpansionlayouthd.json')),
        'playerinventoryexpansionlayouthd.json': inventory_layout(load(romfs, 'playerinventoryexpansionlayouthd.json')),
        'hirelinginventorypanelhd.json': merc_layout(load(romfs, 'hirelinginventorypanelhd.json'), load(romfs, 'playerinventoryoriginallayouthd.json')),
        'videooptionspanelhd.json': custom_options(load(romfs, 'videooptionspanelhd.json'), load(romfs, '_profilehd.json')),
        'chroniclepanelhd.json': chronicle_layout(romfs),
        'controller/messagelogpanelhd.json': rift_layout(romfs),
        'vendorpanellayouthd.json': vendor_layout(load(romfs, 'vendorpanellayouthd.json')),
        'charactercreatepanelhd.json': character_layout(load(romfs, 'charactercreatepanelhd.json')),
        'hudmonsterhealthhd.json': monster_health(load(romfs, 'hudmonsterhealthhd.json')),
        'controller/controlleroverlayhd.json': controller_shell(load(romfs, 'controller/controlleroverlayhd.json')),
    }
    for rel in ('pauselayouthd.json', 'pauselayoutgardenhd.json'):
        changes[rel] = pause_layout(load(romfs, rel))
    changes.update(desktop_profiles(romfs))
    for rel, page in changes.items():
        for w in walk(page):
            assert len(w.get('children', [])) <= 256, w.get('name')
            # StackWidget del tablero upstream repite su separador sin estado.
            names = [c['name'] for c in w.get('children', []) if c.get('name') and c['name'] != 'spacer']
            assert len(names) == len(set(names)), (rel, w.get('name'), names)
        dst = outdir / rel.replace('/', '_')
        dst.write_text(json.dumps(page, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
        result[LAY + rel] = str(dst)
    return result


def emit(romfs, output, replacements, workdir, tag):
    if output.exists():
        raise FileExistsError('El overlay ya existe: ' + str(output))
    manifest = {line.split('|')[9].lower().split(':', 1)[-1] for line in (romfs / 'data/build_manifest.psv').read_text().splitlines() if len(line.split('|')) >= 11}
    table = workdir / (tag + '_repl.tsv')
    table.write_text(''.join(('' if k.lstrip('+') in manifest else '+') + k.lstrip('+') + '\t' + str(v) + '\n' for k, v in replacements.items()), encoding='utf-8')
    packed = workdir / (tag + '_packed')
    subprocess.run([sys.executable, str(HERE / '../../tools/casc_overlay.py'), str(romfs), str(packed), str(table)], check=True)
    data = [p for p in (packed / 'data').glob('*.data') if not p.is_symlink()]
    assert len(data) == 1
    (output / 'data').mkdir(parents=True)
    for p in [packed / 'data/.build.config', *data]:
        shutil.copyfile(p, output / 'data' / p.name)
    (output / 'pc_desktop').write_text('Layouts PC originales; fallback nativo si el archivo raíz no existe.\n', encoding='utf-8')
    (output / 'pc_manifest.json').write_text(json.dumps({k.lstrip('+'): hashlib.sha256(Path(v).read_bytes()).hexdigest() for k, v in replacements.items()}, indent=1) + '\n')
    print('Overlay PC:', output, data[0].name, data[0].stat().st_size, 'bytes', flush=True)


@lru_cache(maxsize=4)
def overlay_index(romfs, folder):
    sys.path.insert(0, str(HERE / '../../tools'))
    import casc_overlay as casc
    original = casc.load_vfs1(romfs)
    tvfs = casc.load_vfs1(folder)
    po, ps, fo, fs, co, cs = struct.unpack_from('>IIIIII', tvfs, 12)
    files = {p.decode().lower(): v for _, p, v, sub, _ in casc.walk_paths(tvfs, po, po + ps) if sub is None}
    so, sl = struct.unpack_from('>II', tvfs, 38)
    specs = tvfs[so:so + sl].split(b'\0')
    return casc, original, tvfs, files, fo, co, cs, specs, casc.read_manifest(romfs)


def read_overlay(romfs, folder, logical):
    casc, original, tvfs, files, fo, co, cs, specs, manifest = overlay_index(romfs, folder)
    if logical.lower() in files:
        at = fo + files[logical.lower()]
        offset = co + int.from_bytes(tvfs[at + 9:at + 9 + casc.offset_field_size(cs)], 'big')
    else:
        oc, size = struct.unpack_from('>II', original, 28)
        offset = co + casc.cft_position(original, oc, oc + size, manifest, logical) - oc
    key = tvfs[offset:offset + 16]
    size = struct.unpack_from('>I', tvfs, offset + 16)[0]
    spec = specs[tvfs[offset + 20]]
    path = folder / 'data' / ('%02d-%08d.data' % (key[9], key[10]))
    if not path.exists():
        path = romfs / 'data' / path.name
    with path.open('rb') as f:
        f.seek(int.from_bytes(key[11:16], 'big'))
        raw = f.read(size)
    if spec in (b'z', b'n'):
        assert hashlib.md5(raw).digest()[:9] == key[:9]
        return zlib.decompress(raw) if spec == b'z' else raw
    return casc.read_data_file(path.parent, bytes(key), size)


def verify(romfs, output):
    import dm_i18n
    assert (output / 'pc_desktop').is_file(), 'Falta pc_desktop'
    assert not (output / 'ctrl_touch').exists(), 'No debe activar layouts táctiles'
    manifest = json.loads((output / 'pc_manifest.json').read_text())
    required_profiles = {LAY + 'controller/_profile' + suffix + '.json' for suffix in CONTROLLER_PROFILES}
    assert required_profiles <= manifest.keys(), 'Faltan perfiles activos PC'
    for logical, digest in manifest.items():
        assert hashlib.sha256(read_overlay(romfs, output, logical)).hexdigest() == digest, logical
    bank = json.loads(read_overlay(romfs, output, LAY + 'bankexpansionlayouthd.json'))
    assert find(bank, 'grid')['fields']['cellCount'] == {'x': 14, 'y': 10}
    assert len([w for w in walk(bank) if w['type'] == 'AdvancedStashSlotWidget']) == 91
    for sub in ('', 'base/'):
        raw = read_overlay(romfs, output, 'data/global/excel/' + sub + 'inventory.bin')
        for row in (10, 25):
            assert raw[4 + row * 240 + 16:4 + row * 240 + 18] == b'\x0e\x0a'
    rows = {r['Key']: r for r in json.loads(read_overlay(romfs, output, 'data/local/lng/strings/ui.json').decode('utf-8-sig'))}
    for key, zh, en, es in dm_i18n._localized_rows():
        assert all(rows[key][lang] == (zh if lang in ('zhCN', 'zhTW') else es if lang in ('esES', 'esMX') else en) for lang in dm_i18n.LANGS), key
    _, original = touch_layout.read_layout(str(romfs), None, suffix='/lng/strings/ui-controller.json')
    before = {r['Key']: r for r in json.loads(original)}
    after = {r['Key']: r for r in json.loads(read_overlay(romfs, output, 'data/local/lng/strings/ui-controller.json').decode('utf-8-sig'))}
    assert before.keys() == after.keys()
    assert [k for k in before if before[k] != after[k]] == ['StrYouDiedConsole']
    _, official = touch_layout.read_layout(str(romfs), None, suffix='/lng/strings/ui.json')
    death = next(r for r in json.loads(official) if r['Key'] == 'StrYouDied')
    assert after['StrYouDiedConsole']['id'] == before['StrYouDiedConsole']['id']
    assert all(after['StrYouDiedConsole'][lang] == death[lang] for lang in dm_i18n.LANGS)
    # HUD y perfiles PC mantienen exactamente las métricas y el arte originales.
    for rel in ('hudpanelhd.json', '_profilehd.json', 'settingspanelhd.json', 'playerinventoryoriginallayouthd.json'):
        assert read_overlay(romfs, output, LAY + rel).decode('utf-8-sig') == touch_layout.read_layout(str(romfs), rel)[1], rel
    for logical in manifest:
        if logical.startswith(LAY):
            page = json.loads(read_overlay(romfs, output, logical))
            contents = json.dumps(page).lower()
            assert all(s not in contents for s in ('ztouch_', 'xtouch_', 'nativehud', 'touchlabel')), logical
            if page.get('basedOn') and '/_profile' not in logical:
                assert page['basedOn'].lower() != logical[len(LAY):], logical
                touch_layout.read_layout(str(romfs), page['basedOn'])
            if '/_profile' in logical:
                target = pc_profile(romfs, profile_family(logical))
                assert all(page.get(k) == v for k, v in target.items()), logical
            for w in walk(page):
                assert len(w.get('children', [])) <= 256, (logical, w.get('name'))
                for value in w.get('fields', {}).values():
                    for filename in value if isinstance(value, list) else [value]:
                        if isinstance(filename, str) and filename.startswith(ART):
                            asset = 'data/hd/global/ui/' + filename.lower() + '.sprite'
                            assert asset in manifest, (logical, asset)
                            sprite = read_overlay(romfs, output, asset)
                            assert sprite[:4] == b'SpA1' and struct.unpack_from('<H', sprite, 4)[0] == 61, asset
    print('Verificación PC: CASC íntegro, 14×10, 91 objetos, %d traducciones, HUD/perfiles PC originales' % len(dm_i18n._localized_rows()), flush=True)


def verify_variants(romfs, output, official_levels=None):
    import restore_pc_level_names
    variant = Path(str(output) + '_variants') / 'item_notes'
    base = json.loads((output / 'pc_manifest.json').read_text())
    notes = json.loads((variant / 'pc_manifest.json').read_text())
    different = {k for k in set(base) | set(notes) if base.get(k) != notes.get(k)}
    assert len(different) == 5 and all(k.startswith('data/local/lng/strings/') for k in different), different
    original, levels = restore_pc_level_names.source(romfs)
    actual = read_overlay(romfs, variant, restore_pc_level_names.LOGICAL)
    assert read_overlay(romfs, output, restore_pc_level_names.LOGICAL) == original
    if official_levels is None:
        official_levels = actual == original
    if official_levels:
        assert actual == original, 'PC requiere nombres oficiales de zonas en los 13 idiomas'
    else:
        restore_pc_level_names.validate_source(original, actual, levels)
    count = {'esES': 0, 'esMX': 0}
    for logical in different:
        a = {r['Key']: r for r in json.loads(read_overlay(romfs, output, logical).decode('utf-8-sig'))}
        b = {r['Key']: r for r in json.loads(read_overlay(romfs, variant, logical).decode('utf-8-sig'))}
        assert set(a) == set(b), logical
        for key, row in a.items():
            for lang, value in row.items():
                actual = b[key][lang]
                if lang in count and actual != value:
                    count[lang] += 1
                    assert not re.search(r'[\u3400-\u9fff]', actual), (logical, key, lang)
                elif lang not in ('zhCN', 'esES', 'esMX'):
                    assert actual == value, (logical, key, lang)
    expected = 1079 if official_levels else 1197
    assert count == {'esES': expected, 'esMX': expected}, count
    assert (variant / 'dm_textbox_plain.tsv').is_file()
    print('Variantes: mismos layouts/datos/arte; %d anotaciones por español; zonas %s' %
          (expected, 'oficiales en los 13 idiomas' if official_levels else 'anotadas en el overlay anterior'), flush=True)


def main(romfs, output):
    import dm_i18n
    import annot_strings
    import drop_light
    import restore_pc_level_names
    romfs, output = Path(romfs), Path(output)
    if not os.environ.get('DM_OVERLAY_MAC'):
        raise SystemExit('DM_OVERLAY_PC requiere DM_OVERLAY_MAC=1')
    if output.exists() or Path(str(output) + '_variants').exists():
        raise FileExistsError('El destino debe ser nuevo para conservar overlays congelados')
    with tempfile.TemporaryDirectory(prefix='d2r-pc-overlay-') as tmp:
        work = Path(tmp)
        repl = build_data(romfs, work)
        repl.update(build_layouts(romfs, work))
        repl.update(build_art(romfs, work))
        evidence = os.environ.get('DM_PC_EVIDENCE_DIR')
        if evidence:
            evidence = Path(evidence)
            evidence.mkdir(parents=True, exist_ok=True)
            for path in work.glob('*.png'):
                shutil.copyfile(path, evidence / path.name)
            for logical, path in repl.items():
                if logical.startswith(LAY):
                    shutil.copyfile(path, evidence / Path(path).name)
        repl.update(dm_i18n.build(str(romfs), str(work), repl)[0])
        repl.update(desktop_strings(romfs, work))
        repl.update(annot_strings.spanish_class_skill_template(romfs, work, repl))
        repl.update(drop_light.build(str(romfs), str(work), repl)[0])
        emit(romfs, output, repl, work, 'base')
        print('Nombres de colección PC (base):', annot_strings.write_collection_names(str(romfs),str(output),repl),flush=True)
        verify(romfs, output)
        notes, stats = annot_strings.build(str(romfs), str(work), repl)
        notes, restored = restore_pc_level_names.restore_generated(romfs, work, notes)
        stats['Nombres oficiales de zonas'] = restored
        variant = Path(str(output) + '_variants') / 'item_notes'
        emit(romfs, variant, dict(repl, **notes), work, 'notes')
        print('Nombres de colección PC (notas):', annot_strings.write_collection_names(str(romfs),str(variant),dict(repl,**notes)),flush=True)
        annot_strings.write_textbox_table(str(variant))
        (variant / 'features.txt').write_text('item_notes\n' + ''.join(k + '\n' for k in sorted(notes)))
        verify(romfs, variant)
        verify_variants(romfs, output, official_levels=True)
        print('Anotaciones:', stats, flush=True)


if __name__ == '__main__':
    main(*sys.argv[1:3])
