"""Crear personaje PC conserva el argumento offline requerido por el ejecutable Switch."""
import os
from pathlib import Path
import copy,json,tempfile,unittest
import pc_desktop_layout as pc
import touch_layout
ROMFS=Path(str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS'))
class CharacterCreate(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.original=pc.load(ROMFS,'characterselectpanelhd.json')
  with tempfile.TemporaryDirectory() as tmp:
   resources=pc.build_layouts(ROMFS,Path(tmp));logical=pc.LAY+'characterselectpanelhd.json'
   cls.actual=json.loads(Path(resources[logical]).read_text()) if logical in resources else cls.original
 def test_button_has_valid_offline_argument(self):
  self.assertEqual(pc.find(self.actual,'CreateCharacter')['fields']['onClickMessage'],'FrontEndNavigation:ToCharacterCreate:1')
 def test_reuses_upstream_contract_without_other_frontend_changes(self):
  _,raw=touch_layout.read_layout(str(ROMFS),'characterselectpanelhd.json')
  upstream=pc.parse(touch_layout._pc_charselect(raw)[0]);expected=copy.deepcopy(self.original)
  message=pc.find(upstream,'CreateCharacter')['fields']['onClickMessage'];self.assertEqual(message,'FrontEndNavigation:ToCharacterCreate:1')
  pc.find(expected,'CreateCharacter')['fields']['onClickMessage']=message
  self.assertEqual(self.actual,expected,'Sólo cambia el argumento: conserva lista, botones y geometría PC originales')
if __name__=='__main__':unittest.main()
