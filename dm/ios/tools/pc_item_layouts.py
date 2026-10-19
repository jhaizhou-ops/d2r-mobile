#!/usr/bin/env python3
# PC 银行/编年史布局：从玩家自己的资源生成，不包含生成内容。
# PC bank/Chronicle layouts: generated from the player's own resources; no generated content is included.
import copy
import json
import re
import struct
from pathlib import Path
import touch_layout
LAY='data/global/ui/layouts/'
ART='PANEL/Zdesktop_pc/'
def parse(text):
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

def clean_selection(widget):
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
    output.paste(source.crop((0, 0, 241, 108)), (0, 0))
    output.paste(source.crop((200, 0, 396, 108)), (241, 0))
    output.paste(source.crop((241, 0, 581, 108)), (437, 0))
    output.paste(source.crop((0, 108, 241, 600)), (0, 108))
    output.paste(source.crop((45, 108, 241, 600)), (241, 108))
    output.paste(source.crop((241, 108, 581, 600)), (437, 108))
    output.paste(source.crop((0, 600, 185, 753)), (0, 600))
    output.paste(source.crop((185, 600, 395, 753)), (283, 600))
    output.paste(source.crop((395, 600, 581, 753)), (591, 600))
    strip = (footer_source or source).crop((185, 600, 195, 753))
    tile_horizontal(output, strip, 185, 600, 98)
    tile_horizontal(output, strip, 493, 600, 98)
    return output

def vault_bottom(source, widened):
    bottom = widened.crop((0, 600, 777, 753))
    strip = source.crop((185, 656, 195, 720))
    tile_horizontal(bottom, strip, 288, 56, 206)
    return bottom

def inventory_layout(original):
    import pouch_layout
    page=copy.deepcopy(original)
    additions={'children':parse('{"children":['+touch_layout._RW+']}')['children']+
                          parse('{"children":['+pouch_layout.widgets()+']}')['children']}
    clean_selection(additions)
    page['children']+=additions['children']
    pouch=find(page,'TouchPouch')['fields']
    pouch.update(filename=ART+'PouchBox')
    pouch['rect']={'x':90,'y':1344,'width':280,'height':100}
    find(page,'TouchPouchIcon')['fields']['rect']={'x':106,'y':1352,'scale':0.8}
    find(page,'TouchPouchCount')['fields']['rect'].update(x=204,y=1344,height=100)
    find(page,'TouchStore')['fields']['rect']={'x':800,'y':1348,'width':366,'scale':0.65}
    return page

def pouch_art(source):
    from PIL import Image
    return source.crop((195,663,391,715)).resize((140,50),Image.Resampling.LANCZOS)

def vendor_layout(original):
    page=copy.deepcopy(original)
    refresh=copy.deepcopy(find(page,'button_refresh'))
    refresh['name']='TouchShopRefresh'
    refresh['fields']['rect']={'x':120,'y':72,'scale':0.55}
    refresh['fields']['onClickMessage']='PanelManager:ClosePanel:TouchShopRefresh'
    page['children'].append(refresh)
    return page

def merc_layout(original, player):
    import merc_slots
    page=copy.deepcopy(original)
    for name in merc_slots.ALL:
        slot=copy.deepcopy(find(player,'slot_'+name))
        slot['fields']['isHireable']=True
        if name in merc_slots.NEW:
            page['children'].append(slot)
            page['children'].append({'type':'RectangleWidget','name':'TouchMercLock_'+name,'fields':{
                'rect':dict(slot['fields']['rect'],x=99999),'color':[0.187,0.019,0.022,0.78]}})
        else:find(page,'slot_'+name)['fields']['rect']=slot['fields']['rect']
    find(page,'Background')['fields']['filename']=ART+'HirelingPanel'
    find(page,'CharacterName')['fields']['rect']['y']=882
    find(page,'HireTypeText')['fields']['rect'].update(x=700,y=882,width=320,height=50)
    find(page,'XpProgress')['fields']['rect']['y']=935
    for name in ('Skill0','Skill1','Skill2'):find(page,name)['fields']['rect']['y']=974
    return page

def inventory_data(raw):
    table=bytearray(raw)
    for row in (10,25):
        at=4+row*240+16
        if table[at:at+2]!=b'\x0a\x0a':raise ValueError('Unexpected native stash dimensions')
        table[at]=14
    # Same existing mercenary rows and player-relative offsets as wide_stash.build.
    for row,dx,dy in ((11,-320,0),(26,-240,60)):
        at=4+row*240+0x28
        if struct.unpack_from('<4i',table,at+4*20)!=(-1,)*4:raise ValueError('Unexpected native mercenary slots')
        for slot in range(4,10):
            left,right,top,bottom,value=struct.unpack_from('<5i',table,4+0x28+slot*20)
            struct.pack_into('<5i',table,at+slot*20,left+dx,right+dx,top+dy,bottom+dy,value)
    return table

def build(romfs, outdir, prior=None):
    import wide_stash
    import romfs_art
    import gen_widestash_art as art
    outdir=Path(outdir)
    outdir.mkdir(parents=True,exist_ok=False)
    result={}
    bank=bank_layout(load(romfs,'bankexpansionlayouthd.json'))
    for w in walk(bank):
        children=w.get('children',[])
        if len(children)>256: raise ValueError('Too many widget children')
        names=[c['name'] for c in children if c.get('name') and c['name']!='spacer']
        if len(names)!=len(set(names)): raise ValueError('Duplicate widget names')
    path=outdir/'bankexpansionlayouthd.json'
    path.write_text(json.dumps(bank,ensure_ascii=False,indent=1)+'\n',encoding='utf-8')
    result[LAY+'bankexpansionlayouthd.json']=str(path)
    inventory=inventory_layout(load(romfs,'playerinventoryexpansionlayouthd.json'))
    path=outdir/'playerinventoryexpansionlayouthd.json'
    path.write_text(json.dumps(inventory,ensure_ascii=False,indent=1)+'\n',encoding='utf-8')
    result[LAY+'playerinventoryexpansionlayouthd.json']=str(path)
    for relative,page in (
        ('vendorpanellayouthd.json',vendor_layout(load(romfs,'vendorpanellayouthd.json'))),
        ('hirelinginventorypanelhd.json',merc_layout(load(romfs,'hirelinginventorypanelhd.json'),load(romfs,'playerinventoryoriginallayouthd.json')))):
        path=outdir/relative
        path.write_text(json.dumps(page,ensure_ascii=False,indent=1)+'\n',encoding='utf-8')
        result[LAY+relative]=str(path)
    library=art.lib()
    paths={'StashPersonal':'PANEL/Stash/StashPanel_BG_Personal','StashShared':'PANEL/Stash/StashPanel_BG_Shared'}
    raws={name:romfs_art.read(str(romfs),'hd/global/ui/'+rel+'.sprite') for name,rel in paths.items()}
    images={name:art.decode(library,raw) for name,raw in raws.items()}
    def save(name,image,header):
        target=outdir/(name+'.sprite')
        target.write_bytes(art.pack(library,header,art.bc3(image),image.width,image.height,image.width))
        result['data/hd/global/ui/'+ART.lower()+name.lower()+'.sprite']=str(target)
    for name in paths:
        image=widen_pc_stash(images[name],images['StashPersonal'])
        save(name,image,raws[name])
        if name=='StashPersonal':
            save('StashTrove',image,raws[name])
            save('VaultBottom',vault_bottom(images[name],image),raws[name])
    inventory_raw=romfs_art.read(str(romfs),'hd/global/ui/PANEL/Inventory/Background.sprite')
    inventory_image=art.decode(library,inventory_raw)
    if inventory_image.size!=(581,753):raise ValueError('Unexpected native inventory background dimensions')
    save('PouchBox',pouch_art(inventory_image),inventory_raw)
    merc_raw=romfs_art.read(str(romfs),'hd/global/ui/PANEL/Hireling/HirelingPanel.sprite')
    merc_image=art.decode(library,merc_raw)
    if merc_image.size!=(581,753):raise ValueError('Unexpected native mercenary background dimensions')
    merc_image.paste(inventory_image.crop((35,80,550,440)),(35,80))
    merc_image.paste(art.decode(library,merc_raw).crop((35,405,550,445)),(35,440))
    save('HirelingPanel',merc_image,merc_raw)
    for sub in ('','base/'):
        logical,raw=wide_stash._entry(str(romfs),'/global/excel/'+sub+'inventory.bin')
        table=inventory_data(raw)
        target=outdir/('inventory_'+sub.replace('/','_')+'.bin')
        target.write_bytes(table);result[logical]=str(target)
    return result,{'layouts':4,'stash_columns':14,'advanced_slots':91,'mercenary_extra_slots':6,'pc_items_requires_profile':True}
