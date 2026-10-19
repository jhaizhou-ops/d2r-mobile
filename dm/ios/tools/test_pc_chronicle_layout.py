"""Cabecera PC de Crónica: arte original, rótulos oficiales y buscador separado."""
import json,struct,tempfile,unittest
import os
from pathlib import Path
from PIL import ImageFont
import pc_desktop_layout as pc
import romfs_art,touch_layout
ROMFS=Path(str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS'))
def profile_size(profile,token):
    seen=set()
    while isinstance(token,str) and token.startswith('$'):
        if token in seen:raise ValueError('Ciclo de tokens de fuente')
        seen.add(token);token=profile[token[1:]]
    return token
class ChronicleHeader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();mapping=pc.build_layouts(ROMFS,Path(cls.tmp.name));cls.mapping=mapping
        cls.page=json.loads(Path(mapping[pc.LAY+'chroniclepanelhd.json']).read_text())
        cls.tabs=pc.find(cls.page,'ChronicleTabs')['fields'];cls.base=pc.load(ROMFS,'chroniclebaselistpanelhd.json')
        _,text=touch_layout.read_layout(str(ROMFS),'',suffix='/lng/strings/ui.json');cls.strings={row['Key']:row for row in json.loads(text)}
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def test_art_width_matches_cell_and_header_never_intersects_search(self):
        tabs=self.tabs;raw=romfs_art.read(str(ROMFS),'hd/global/ui/'+tabs['filename'].replace('\\','/')+'.sprite')
        frame_width=struct.unpack_from('<H',raw,6)[0]*2
        self.assertEqual(tabs['tabSize']['x'],frame_width)
        profile=pc.pc_profile(ROMFS,'hd');rect=tabs['rect'];self.assertIsInstance(rect,dict)
        width=rect['width'];group=4*tabs['tabSize']['x']+3*tabs['tabPadding']['x']
        right=(width+group)/2
        search=profile['ChronicleSearchBox'];self.assertGreaterEqual(search['x']-right,24)
        self.assertGreaterEqual((width-group)/2,0)
        self.assertEqual(profile['OptionsPanelRect']['y']+search['y'],rect['y'])
    def test_four_original_actions_and_child_layouts_remain(self):
        self.assertEqual(self.tabs['tabCount'],4)
        self.assertEqual(self.tabs['textStrings'],['@Unique','@Sets','@Runewords','@Bosses'])
        self.assertEqual(self.tabs['tabMessages'],['ChroniclePanelMessage:ToggleChildPanel:'+p for p in ('ChronicleUniquesListPanel','ChronicleSetsListPanel','ChronicleRunewordsListPanel','TouchKillBoard')])
        self.assertIsNotNone(pc.find(self.page,'TouchKillBoard'))
        self.assertEqual(pc.find(self.base,'SearchInput')['fields']['maxStringLength'],32)
    def test_all_spanish_labels_fit_original_font_at_three_viewports_and_retina(self):
        style=self.tabs['textStyle'];self.assertEqual(style['fontFace'],'Exocet');self.assertFalse(self.tabs['truncateText'])
        path=romfs_art.font(str(ROMFS),'ExocetBlizzardOT-Medium.otf');width=self.tabs['tabSize']['x']
        # Reservar el borde y el margen interior sin asumir que kerning sea
        # una escala horizontal de todos los píxeles del glifo.
        inset=20
        for family in ('hd','lv','asian'):
            profile=pc.pc_profile(ROMFS,family);size=profile_size(profile,style['pointSize'])
            font=ImageFont.truetype(path,size)
            for lang in ('esES','esMX'):
                for key in self.tabs['textStrings']:
                    value=self.strings[key[1:]][lang]
                    with self.subTest(family=family,lang=lang,label=value):
                        box=font.getbbox(value);extent=max(font.getlength(value),box[2]-min(0,box[0]))
                        self.assertLessEqual(extent,width-2*inset)
                        for vw,vh in ((1280,720),(2560,1440),(3024,1700)):
                            for backing in (1,2):
                                scale=min(vw/3840,vh/2160)*backing;raster=ImageFont.truetype(path,max(1,round(size*scale)))
                                # Dos bordes rasterizados pueden variar hasta
                                # dos píxeles al redondear el tamaño de fuente.
                                box=raster.getbbox(value);extent=max(raster.getlength(value),box[2]-min(0,box[0]))
                                self.assertLessEqual(extent,(width-2*inset)*scale+2)
                                box=raster.getbbox(value);self.assertLessEqual(box[3]-box[1],(self.tabs['tabSize']['y']-16)*scale)
    def test_search_widget_geometry_and_hint_are_byte_for_byte_original(self):
        for rel in ('chroniclebaselistpanelhd.json','chronicleuniqueslistpanelhd.json','chroniclesetslistpanelhd.json','chroniclerunewordslistpanelhd.json'):
            self.assertNotIn(pc.LAY+rel,self.mapping)
        self.assertEqual(pc.find(self.base,'SearchInput')['fields']['hint']['text'],'@ChronicleSearchPlaceholder')
        for key in ('ChronicleSearchBox','ChronicleSearchInput','ChronicleLeftCursorPadding'):
            original=pc.pc_profile(ROMFS,'hd')[key]
            for profile in pc.desktop_profiles(ROMFS).values():self.assertEqual(profile[key],original)
if __name__=='__main__':unittest.main()
