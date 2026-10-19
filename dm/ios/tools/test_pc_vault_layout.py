"""Geometría PC de la cámara, métricas españolas originales y sprites del aro."""
import os
from pathlib import Path
import tempfile
import unittest
from PIL import Image, ImageDraw, ImageFont
import dm_i18n
import pc_desktop_layout as pc
import romfs_art
import touch_layout

ROMFS = Path(str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS'))

class DesktopVault(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bank = pc.bank_layout(pc.load(ROMFS, 'bankexpansionlayouthd.json'))
        cls.vault = pc.find(cls.bank, 'TouchVault')

    def test_opaque_mask_covers_entire_native_grid(self):
        self.assertFalse(any(w.get('name') == 'PCVaultContent' for w in pc.walk(self.vault)))
        mask = pc.find(self.vault, 'TouchV_Back')['fields']
        a = mask['rect'];grid = pc.find(self.bank, 'grid')['fields'];b = dict(grid['rect'], width=14*98, height=10*98)
        self.assertLessEqual(a['x'], b['x']);self.assertLessEqual(a['y'], b['y'])
        self.assertGreaterEqual(a['x']+a['width'], b['x']+b['width'])
        self.assertGreaterEqual(a['y']+a['height'], b['y']+b['height'])
        self.assertEqual(mask['backgroundColor'][3], 1)
        self.assertEqual(pc.find(self.vault, 'TouchV_Cover')['fields']['rect'], {'x': 0, 'y': 1200})

    def test_rows_and_filter_columns_never_overlap_at_two_viewports(self):
        names=('TouchV_Top','TouchV_Parts','TouchV_Stats','TouchV_Tiers','TouchV_Uniq')
        for width,height in ((1280,720),(3024,1700)):
            scale=min(width/3840,height/2160)
            rects=[pc.find(self.vault,n)['fields']['rect'] for n in names]
            for a,b in zip(rects,rects[1:]):
                with self.subTest(viewport=(width,height), row=b):
                    self.assertGreaterEqual((b['y']-a['y']-a['height'])*scale,12*scale)
            tiers=pc.find(self.vault,'TouchV_Tiers')
            for i,key in enumerate(('NormalQuality','ExceptionalQuality','EliteQuality')):
                checkbox=pc.find(tiers,'TouchV_T%d'%i)['fields']['rect']
                label=pc.find(tiers,'TouchV_T%d_Text'%i)['fields']
                self.assertEqual(label['text'],'@'+key)
                self.assertFalse(label.get('fitToText',False))
                self.assertEqual(label['style']['pointSize'],'$MediumFontSize')
                r=label['rect'];self.assertGreaterEqual(r['x']-checkbox['x']-checkbox['width'],10)
                if i<2:
                    next_box=pc.find(tiers,'TouchV_T%d'%(i+1))['fields']['rect']
                    self.assertGreaterEqual((next_box['x']-r['x']-r['width'])*scale,10*scale)

    def test_category_two_lines_fit_native_font_without_truncation(self):
        translations=dm_i18n.spanish_rows();draw=ImageDraw.Draw(Image.new('L',(1,1)))
        for family in ('hd','lv','asian'):
            profile=pc.pc_profile(ROMFS,family)
            for i in range(11):
                part=pc.find(self.vault,'TouchV_PT%d'%i);frame=pc.find(part,'Hit')['fields']['rect']
                text=pc.find(part,'Text')['fields'];r=text['rect'];style=text['style']
                self.assertEqual(pc.find(part,'TextOn')['fields']['rect'],r)
                self.assertEqual(pc.find(part,'Bg')['fields']['rect'],frame)
                self.assertEqual(pc.find(part,'BgActive')['fields']['rect'],frame)
                self.assertFalse(text['truncateWithEllipsis'])
                self.assertEqual(style['options']['newlineHandling'],'standard')
                self.assertLessEqual(r['x']+r['width'],frame['width']-8)
                self.assertLessEqual(r['y']+r['height'],frame['height']-8)
                size=profile[style['pointSize'][1:]];font=ImageFont.truetype(romfs_art.font(str(ROMFS),'exocetblizzardot-medium.otf'),size)
                value=translations[text['text'][1:]].replace('\\n','\n')
                kerning=profile[style['spacing'][1:]]['kerning']
                for line in value.split('\n'):
                    self.assertLessEqual(font.getlength(line)*kerning,r['width']-8)
                box=draw.multiline_textbbox((0,0),value,font=font,spacing=0)
                self.assertLessEqual(box[3]-box[1],r['height']-8)

    def test_capacity_names_and_messages_are_preserved(self):
        import wide_stash
        old=wide_stash.vault_widgets()
        for prefix,count in (('TouchV_U',12),('TouchV_S',7)):
            for i in range(count):self.assertIsNotNone(pc.find(self.vault,prefix+str(i)))
        for i in range(7):
            row=pc.find(self.vault,'TouchV_S%d'%i)
            for j in range(6):self.assertIsNotNone(pc.find(row,'Tile%d'%j))
        old_messages={w['name']:w['fields']['onClickMessage'] for w in pc.walk(old) if 'onClickMessage' in w.get('fields',{})}
        for name,message in old_messages.items():self.assertEqual(pc.find(self.vault,name)['fields']['onClickMessage'],message)
        self.assertEqual(pc.find(self.vault,'TouchV_Search')['fields']['maxStringLength'],16)

    def test_page_counter_fits_full_official_spanish_at_native_sizes(self):
        import math,re
        header=Path('dm/host/dm_vault_tab.h').read_text()
        maximum=math.ceil(len(re.findall(r'^\s*\{ [01],',header,re.M))/12)
        self.assertEqual(maximum,47)
        page=pc.find(self.vault,'TouchV_Page')['fields'];r=page['rect'];style=page['style']
        _,raw=touch_layout.read_layout(str(ROMFS),'',suffix='/lng/strings/ui.json')
        official=next(row for row in __import__('json').loads(raw) if row['Key']=='SharedStashPage')
        self.assertFalse(page['truncateWithEllipsis'])
        self.assertFalse(style['options']['lineWrap'])
        left=pc.find(self.vault,'TouchV_PgL')['fields']['rect'];right=pc.find(self.vault,'TouchV_PgR')['fields']['rect']
        self.assertGreaterEqual(r['x']-left['x']-94*left['scale'],16)
        self.assertGreaterEqual(right['x']-r['x']-r['width'],16)
        fonts=[f[9].split('/')[-1] for line in (ROMFS/'data/build_manifest.psv').read_text().splitlines() if len(f:=line.split('|'))>9 and '/hd/ui/fonts/' in f[9].lower() and f[9].lower().endswith(('.ttf','.otf'))]
        self.assertEqual(len(fonts),11)
        for family in ('hd','lv','asian'):
            profile=pc.pc_profile(ROMFS,family);size=profile[style['pointSize'][1:]];kerning=profile[style['spacing'][1:]]['kerning']
            for filename in fonts:
                path=romfs_art.font(str(ROMFS),filename);font=ImageFont.truetype(path,size)
                rasters=[]
                for width,height in ((1280,720),(2560,1440),(3024,1700)):
                    for backing in (1,2):
                        scale=min(width/3840,height/2160)*backing
                        rasters.append((scale,ImageFont.truetype(path,max(1,round(size*scale)))))
                for lang in ('esES','esMX'):
                    fmt=official[lang].replace('%u','%d')
                    for total in range(1,maximum+1):
                        for current in range(1,total+1):
                            value=fmt%(current,total)
                            for text in (value,value.upper()):
                                advance=font.getlength(text)*kerning
                                with self.subTest(family=family,font=filename,value=text):
                                    self.assertLessEqual(advance,r['width']-16)
                                    for scale,raster in rasters:
                                        self.assertLessEqual(raster.getlength(text)*kerning,(r['width']-16)*scale)

    def test_only_three_cursor_assets_become_transparent(self):
        with tempfile.TemporaryDirectory() as tmp:
            replacements=pc.build_cursor_art(ROMFS,Path(tmp))
            expected={'data/hd/global/ui/controller/cursor/'+s+'.sprite' for s in ('idle','hold','held')}
            self.assertEqual(set(replacements),expected)
            for logical,path in replacements.items():
                source=romfs_art.read(str(ROMFS),logical[5:]);actual=Path(path).read_bytes()
                self.assertEqual(source[:40],actual[:40]);self.assertEqual(len(source),len(actual))
                self.assertFalse(any(actual[40:]))

if __name__ == '__main__': unittest.main()
