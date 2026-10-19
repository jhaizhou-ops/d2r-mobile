"""Paginación PC: flechas personales aisladas y navegación compartida original."""
import os
from pathlib import Path
import unittest
import pc_desktop_layout as pc
ROMFS=Path(str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS'))
class StashPages(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.original=pc.load(ROMFS,'bankexpansionlayouthd.json');cls.bank=pc.bank_layout(cls.original)
 def test_personal_buttons_belong_to_existing_personal_page(self):
  personal=pc.find(self.bank,'TouchPgPage')
  for suffix,action in (('Left','PreviousPage'),('Right','NextPage')):
   button=next((w for w in pc.walk(personal) if w['name']=='TouchStash'+suffix),None)
   self.assertIsNotNone(button,'La flecha personal debe heredar el desplazamiento de su página')
   self.assertEqual(button['fields']['onClickMessage'],'PanelManager:ClosePanel:TouchPersonalStash'+('Previous' if suffix=='Left' else 'Next'))
  self.assertFalse(any(w['name'].startswith('TouchStash') for w in self.bank['children']))
 def test_personal_callbacks_are_explicit_requests(self):
  for suffix,action in (('Left','PreviousPage'),('Right','NextPage')):
   self.assertEqual(pc.find(self.bank,'TouchStash'+suffix)['fields']['onClickMessage'],'PanelManager:ClosePanel:TouchPersonalStash'+('Previous' if suffix=='Left' else 'Next'))
 def test_shared_buttons_keep_original_callbacks_and_geometry(self):
  original=pc.find(self.original,'StashNavigation');actual=pc.find(self.bank,'StashNavigation')
  self.assertEqual(actual['children'],original['children'])
  for suffix in ('Left','Right'):
   self.assertEqual(pc.find(actual,'Stash'+suffix+'Arrow')['fields']['onClickMessage'],'BankPanelMessage:SharedStash'+suffix)
  self.assertEqual(pc.find(actual,'CurrentSharedStashText'),pc.find(original,'CurrentSharedStashText'))
 def test_counter_and_arrow_coordinates_fit_personal_and_leave_shared_clear(self):
  page=pc.find(self.bank,'TouchPgPage');self.assertIsNotNone(pc.find(page,'TouchPgPage_Text'))
  arrows=[next((w for w in pc.walk(page) if w['name']=='TouchStash'+s),None) for s in ('Left','Right')]
  self.assertTrue(all(arrows),'Los hitboxes deben desplazarse con el contador personal')
  for arrow in arrows:
   r=arrow['fields']['rect'];self.assertEqual((r['width'],r['height']),(64,64));self.assertEqual(r['y'],1215)
   for offset in (0,99999):
    x=r['x']+offset;y=r['y'];hit=lambda px,py:x<=px<x+64 and y<=py<y+64
    self.assertEqual(hit(r['x']+32,r['y']+32),offset==0)
  self.assertEqual(pc.find(page,'TouchPgPage_Text')['fields']['text'],'')
if __name__=='__main__':unittest.main()
