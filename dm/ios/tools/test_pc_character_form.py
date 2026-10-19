"""Crear personaje PC: nombre, Elíseo y opciones conservan espacios distintos."""
import copy,json,tempfile,unittest
import os
from pathlib import Path
import pc_desktop_layout as pc
import romfs_art,touch_layout
from PIL import ImageFont
ROMFS=Path(str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS'))
def intersects(a,b):
 return max(a['x'],b['x'])<min(a['x']+a['width'],b['x']+b['width']) and max(a['y'],b['y'])<min(a['y']+a['height'],b['y']+b['height'])
class CharacterForm(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.original=pc.load(ROMFS,'charactercreatepanelhd.json')
  with tempfile.TemporaryDirectory() as tmp:
   mapping=pc.build_layouts(ROMFS,Path(tmp));cls.actual=json.loads(Path(mapping[pc.LAY+'charactercreatepanelhd.json']).read_text())
 def test_elysium_does_not_cover_name_or_input_at_any_viewport(self):
  parent=pc.find(self.actual,'InputContainer')['fields']['rect']
  controls=[]
  for name in ('NameLabel','InputText'):
   rect=copy.deepcopy(pc.find(self.actual,name)['fields']['rect']);rect['x']+=parent['x'];rect['y']+=parent['y'];controls.append(rect)
  # La caja completa de entrada abarca también el marco de arte original.
  controls.append(dict(x=-444,y=1728,width=888,height=142))
  for name in ('TouchBlissTitle','TouchBlissDesc'):
   rect=pc.find(self.actual,name)['fields']['rect']
   for vw,vh in ((1280,720),(2560,1672),(3024,1964)):
    for dpi in (1,2):
     k=min(vw/3840,vh/2160)*dpi
     transformed=lambda r:{key:value*k for key,value in r.items() if key in ('x','y','width','height')}
     for control in controls:
      with self.subTest(widget=name,viewport=(vw,vh),dpi=dpi):self.assertFalse(intersects(transformed(rect),transformed(control)))
 def test_native_creation_controls_and_all_eight_classes_are_unchanged(self):
  for name in ('Camera','Title','InfoContainer','InputContainer','ToMainMenu','ToggleLegacy','ToGame','GameVersionContainer','LoadingScreen'):
   self.assertEqual(pc.find(self.actual,name),pc.find(self.original,name),name)
  classes=[w for w in pc.walk(self.actual) if w['type']=='HDCharacterSelectorWidget'];self.assertEqual(len(classes),8)
  for widget in classes:self.assertEqual(widget,pc.find(self.original,widget['name']))
  self.assertEqual(self.actual['fields'],self.original['fields'])
 def test_elysium_uses_native_checkbox_and_preserves_port_callback(self):
  fields=pc.find(self.actual,'TouchBliss')['fields'];self.assertEqual(fields['onClickMessage'],'PanelManager:ClosePanel:TouchBliss')
  self.assertEqual(fields['filename'],'Lobby\\CreateGame\\CreateGame_AdvancedCheckbox')
  x,y=fields['rect']['x'],fields['rect']['y'];self.assertTrue(-1820<=x<-600 and 1700<=y<1870)
  for name in ('TouchBlissTitle','TouchBlissDesc'):
   rect=pc.find(self.actual,name)['fields']['rect'];self.assertLessEqual(rect['x']+rect['width'],-500);self.assertLessEqual(rect['y']+rect['height'],1920)
 def test_upsell_preserves_callback_and_has_space_when_native_class_allows_it(self):
  version=pc.find(self.actual,'GameVersionContainer')['fields']['rect'];dropdown=copy.deepcopy(pc.find(self.actual,'GameVersionDropdown')['fields']['rect'])
  dropdown['x']+=1920+version['x'];dropdown['y']+=2160+version['y']
  create=dict(x=1208,y=1940,width=700,height=170);name=dict(x=-444,y=1728,width=888,height=142)
  for widget in ('UpsellDescription','UpsellButton'):
   original=copy.deepcopy(pc.find(self.original,widget));actual=copy.deepcopy(pc.find(self.actual,widget));rect=actual['fields'].pop('rect');original['fields'].pop('rect');self.assertEqual(actual,original)
   rect=dict(rect,x=rect['x']+1920,y=rect['y']+2160)
   if widget=='UpsellButton':rect.update(width=700,height=170)
   for other in (dropdown,create,name):self.assertFalse(intersects(rect,other),widget)
   self.assertTrue(-1920<=rect['x']<rect['x']+rect['width']<=1920)
   self.assertTrue(0<=rect['y']<rect['y']+rect['height']<=2160)
  self.assertEqual(pc.find(self.actual,'UpsellButton')['fields']['onClickMessage'],'CharacterCreate:Upsell')
 def test_elysium_description_fits_native_font_in_all_profiles(self):
  # dmBlissDesc procede de la tabla del port, no del UI original.
  fields=next(line.split('\t') for line in Path('dm/i18n/ui_text.tsv').read_text().splitlines() if line.startswith('dmBlissDesc\t'))
  text=fields[3];rect=pc.find(self.actual,'TouchBlissDesc')['fields']['rect']
  for family in ('hd','lv','asian'):
   profile=pc.pc_profile(ROMFS,family);font=ImageFont.truetype(romfs_art.font(str(ROMFS),'Formal436BT.ttf'),profile['MediumFontSize']);lines=['']
   for word in text.split():
    candidate=(lines[-1]+' '+word).strip()
    if font.getlength(candidate)<=rect['width']-20:lines[-1]=candidate
    else:lines.append(word)
   self.assertLessEqual(len(lines)*profile['MediumFontSize'],rect['height']);self.assertTrue(all(font.getlength(line)<=rect['width']-20 for line in lines))
 def test_native_upsell_text_fits_in_both_spanish_locales(self):
  _,raw=touch_layout.read_layout(str(ROMFS),'',suffix='/lng/strings/ui.json');rows={row['Key']:row for row in json.loads(raw)};rect=pc.find(self.actual,'UpsellDescription')['fields']['rect']
  for family in ('hd','lv','asian'):
   size=pc.pc_profile(ROMFS,family)['LargeFontSize']
   for filename in ('ExocetBlizzardOT-Medium.otf','IrisL.ttf','Formal436BT.ttf'):
    font=ImageFont.truetype(romfs_art.font(str(ROMFS),filename),size)
    for locale in ('esES','esMX'):
     lines=['']
     for word in rows['ClassRequiresPurchase'][locale].split():
      candidate=(lines[-1]+' '+word).strip()
      if font.getlength(candidate)<=rect['width']-20:lines[-1]=candidate
      else:lines.append(word)
     with self.subTest(family=family,font=filename,locale=locale):self.assertLessEqual(len(lines)*size*1.2,rect['height']-20)
if __name__=='__main__':unittest.main()
