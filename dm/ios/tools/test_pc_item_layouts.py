# PC 储藏箱布局测试；合成布局不含游戏内容。
# PC stash layout tests; synthetic layouts contain no game content.
import copy
import unittest
import pc_item_layouts as pc


def fixture():
    def widget(name, kind='Widget', fields=None, children=None):
        return {'name':name,'type':kind,'fields':fields or {'rect':{'x':0,'y':0}},'children':children or []}
    children=[widget('grid','InventoryGridWidget',{'cellCount':{'x':10,'y':10},'rect':{'x':89,'y':219}})]
    children += [widget(name) for name in ('gold_amount','gold_withdraw','StashNavigation','PreviousSeasonToggleDisplay','close','click_catcher','title')]
    children += [widget('background','ImageWidget',{'filename':'original'})]
    children += [widget('BankTabs','TabBarWidget',{'tabCount':5})]
    for group in ('advancedstash_gems','advancedstash_materials','advancedstash_runes'):
        children.append(widget(group,children=[]))
    for i in range(91):
        item=widget('slot%02d'%i,'AdvancedStashSlotWidget',{'itemCode':'synthetic%02d'%i,'rect':{'x':0,'y':0,'width':98,'height':196 if i%3==0 else 98}})
        children[-3+i%3]['children'].append(item)
    for name in ('SharedStashLeft','SharedStashRight'):
        children.append(widget(name,'ButtonWidget',{'onClickMessage':'NativeShared:'+name}))
    return widget('BankExpansionLayout','BankPanel',{'backgroundFile':['original']*5},children)


class ItemLayouts(unittest.TestCase):
    def test_bank_preserves_slots_and_shared_callbacks(self):
        original=fixture();before=copy.deepcopy(original);bank=pc.bank_layout(original)
        self.assertEqual(original,before)
        slots=[w for w in pc.walk(bank) if w['type']=='AdvancedStashSlotWidget']
        self.assertEqual(len(slots),91)
        expected={w['fields']['itemCode']:w['fields']['rect']['height'] for w in pc.walk(original) if w['type']=='AdvancedStashSlotWidget'}
        self.assertEqual({w['fields']['itemCode']:w['fields']['rect']['height'] for w in slots},expected)
        used=set()
        for slot in slots:
            r=slot['fields']['rect'];x=(r['x']-89)//98;y=(r['y']-219)//98
            cells={(x,y+i) for i in range(r['height']//98)}
            self.assertFalse(used&cells);used|=cells
            self.assertTrue(all(0<=cx<14 and 0<=cy<10 for cx,cy in cells))
        for name in ('SharedStashLeft','SharedStashRight'):
            self.assertEqual(pc.find(bank,name),pc.find(original,name))

    def test_personal_arrows_have_personal_parent_and_native_parser(self):
        bank=pc.bank_layout(fixture());page=pc.find(bank,'TouchPgPage')
        self.assertEqual({w['name'] for w in page['children']},{'TouchPgPage_Text','TouchStashLeft','TouchStashRight'})
        for side,argument in (('Left','Previous'),('Right','Next')):
            fields=pc.find(page,'TouchStash'+side)['fields']
            self.assertEqual(fields['onClickMessage'],'PanelManager:ClosePanel:TouchPersonalStash'+argument)
            self.assertEqual(fields['rect']['width'],64)
        self.assertEqual(page['fields']['rect']['x'],99999)
        self.assertEqual(pc.find(bank,'BankTabs')['fields']['tabCount'],6)

    def test_wrong_revision_fails_without_mutating_input(self):
        source=fixture();pc.find(source,'grid')['fields']['cellCount']['x']=11
        before=copy.deepcopy(source)
        with self.assertRaises(AssertionError):pc.bank_layout(source)
        self.assertEqual(source,before)

    def test_native_json_parser_preserves_literal_comment_markers(self):
        self.assertEqual(pc.parse('{/* comment */"text":"a//b", "array":[1,],}'),{'text':'a//b','array':[1]})

    def test_art_uses_only_supplied_image_and_preserves_frame(self):
        from PIL import Image
        source=Image.new('RGBA',(581,753),(17,37,59,255))
        result=pc.widen_pc_stash(source)
        self.assertEqual(result.size,(777,753))
        self.assertEqual(result.crop((0,0,185,753)).tobytes(),source.crop((0,0,185,753)).tobytes())
        self.assertEqual(pc.vault_bottom(source,result).size,(777,153))

    def test_inventory_keeps_native_widgets_and_adds_existing_port_callbacks(self):
        original={'type':'PlayerInventoryPanel','name':'PlayerInventoryExpansionLayout','fields':{'nativeOption':17},
                  'children':[{'name':'native_grid','type':'InventoryGridWidget','fields':{'rect':{'x':89,'y':219},'cellCount':{'x':10,'y':4}}}]}
        before=copy.deepcopy(original)
        result=pc.inventory_layout(original)
        self.assertEqual(original,before)
        self.assertEqual(result['fields'],before['fields'])
        self.assertEqual(result['children'][:len(before['children'])],before['children'])
        self.assertEqual(pc.find(result,'TouchPouch')['fields']['onClickMessage'],'PanelManager:ClosePanel:TouchPouch')
        self.assertEqual(pc.find(result,'TouchStore')['fields']['onClickMessage'],'PanelManager:ClosePanel:TouchStore')
        self.assertEqual(pc.find(result,'TouchPouch')['fields']['rect'],{'x':90,'y':1344,'width':280,'height':100})
        self.assertEqual(pc.find(result,'TouchPouch')['fields']['filename'],pc.ART+'PouchBox')
        self.assertEqual(pc.find(result,'TouchStore')['fields']['rect']['x'],800)
        self.assertEqual(len([w for w in pc.walk(result) if w.get('name')=='TouchPouchBox']),1)

    def test_pouch_art_uses_original_inventory_gold_box(self):
        from PIL import Image
        source=Image.new('RGBA',(581,753),(17,37,59,255))
        source.putpixel((195,663),(255,0,0,255))
        before=source.tobytes()
        result=pc.pouch_art(source)
        self.assertEqual(source.tobytes(),before)
        expected=source.crop((195,663,391,715)).resize((140,50),Image.Resampling.LANCZOS)
        self.assertEqual(result.tobytes(),expected.tobytes())
        self.assertEqual(result.size,(140,50))

    def test_vendor_refresh_preserves_native_purchase_and_repair_controls(self):
        original={'name':'VendorPanelLayout','children':[{'name':name,'type':'ButtonWidget','fields':{
            'onClickMessage':'Native:'+name,'rect':{'x':900,'y':1200},'filename':'native-refresh'}}
            for name in ('button_refresh','button_repair','button_repair_all')]}
        before=copy.deepcopy(original);page=pc.vendor_layout(original)
        self.assertEqual(original,before)
        self.assertEqual(page['children'][:3],before['children'])
        self.assertEqual(pc.find(page,'TouchShopRefresh')['fields']['onClickMessage'],'PanelManager:ClosePanel:TouchShopRefresh')

    def test_mercenary_adds_six_native_slots_and_unlock_masks(self):
        import merc_slots
        def widget(name,fields=None):return {'name':name,'type':'InventorySlotWidget','fields':fields or {'rect':{'x':1,'y':2,'width':64,'height':64},'nativeField':17}}
        original={'name':'HirelingInventoryPanel','children':[widget('slot_'+name) for name in merc_slots.ALL if name not in merc_slots.NEW]}
        original['children'] += [widget(name,{'rect':{'x':0,'y':0},'filename':'native'}) for name in ('Background','CharacterName','HireTypeText','XpProgress','Skill0','Skill1','Skill2')]
        player={'children':[widget('slot_'+name) for name in merc_slots.ALL]}
        before=copy.deepcopy(original);page=pc.merc_layout(original,player)
        self.assertEqual(original,before)
        for name in merc_slots.ALL:
            slot=pc.find(page,'slot_'+name)
            self.assertEqual(slot['fields']['nativeField'],17)
            self.assertEqual(slot['fields']['rect'],pc.find(player,'slot_'+name)['fields']['rect'])
        for name in merc_slots.NEW:
            self.assertTrue(pc.find(page,'slot_'+name)['fields']['isHireable'])
            self.assertEqual(pc.find(page,'TouchMercLock_'+name)['fields']['rect']['x'],99999)

    def test_inventory_tables_keep_unrelated_bytes_and_add_upstream_merc_cells(self):
        import struct
        raw=bytearray(4+27*240)
        for row in (10,25):raw[4+row*240+16:4+row*240+18]=b'\x0a\x0a'
        for row in (11,26):struct.pack_into('<4i',raw,4+row*240+0x28+4*20,*([-1]*4))
        struct.pack_into('<5i',raw,4+0x28+4*20,10,20,30,40,50)
        before=bytes(raw);table=pc.inventory_data(raw)
        self.assertEqual(bytes(raw),before)
        self.assertEqual(table[4+10*240+16],14)
        self.assertEqual(table[4+25*240+16],14)
        self.assertEqual(struct.unpack_from('<5i',table,4+11*240+0x28+4*20),(-310,-300,30,40,50))
        self.assertEqual(struct.unpack_from('<5i',table,4+26*240+0x28+4*20),(-230,-220,90,100,50))
        allowed={4+row*240+16 for row in (10,25)}
        for row in (11,26):allowed.update(range(4+row*240+0x28+4*20,4+row*240+0x28+10*20))
        self.assertTrue(all(i in allowed or table[i]==before[i] for i in range(len(table))))


if __name__=='__main__':unittest.main()
