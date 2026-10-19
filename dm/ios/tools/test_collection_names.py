"""Nombres de presentación de colección, derivados de claves oficiales españolas."""
import json,tempfile,unittest
import os
from pathlib import Path
import annot_strings as annot
import pc_desktop_layout as pc
import touch_layout
ROMFS=Path(str(Path(os.environ.get('DM_ROOT', Path.home() / 'dm-work')) / 'data/switch/3.3.94012/Update/RomFS'))
@unittest.skipUnless(ROMFS.is_dir() and 'DM_TEST_COLLECTION_OVERLAY' in os.environ, 'Requiere RomFS propio y overlay de referencia')
class CollectionNames(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.dir=Path(cls.tmp.name);cls.maps={};cls.rows={}
        for variant in ('base','item_notes'):
            overlay=Path(os.environ['DM_TEST_COLLECTION_OVERLAY']+('_variants/item_notes' if variant=='item_notes' else ''));repl={};rows={}
            for name in ('item-names.json','item-runes.json','chinese-overlay.json'):
                logical,_=touch_layout.read_layout(str(ROMFS),'',suffix='/lng/strings/'+name);raw=pc.read_overlay(ROMFS,overlay,logical);out=cls.dir/(variant+'-'+name);out.write_bytes(raw);repl[logical.lower()]=str(out)
                for row in json.loads(raw):rows[row['Key']]=row
            cls.maps[variant]=annot.collection_presentation_map(str(ROMFS),repl);cls.rows[variant]=rows
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def test_exact_official_names_for_real_qa42_rows(self):
        _,raw=touch_layout.read_layout(str(ROMFS),'',suffix='/lng/strings/item-names.json');expected={row['Key']:row for row in json.loads(raw)}
        for variant,mapping in self.maps.items():
            for key in ('vgl','xvg','xtp','uvg'):
                for lang in ('esES','esMX'):
                    source=self.rows[variant][key][lang]
                    self.assertEqual(mapping[source],annot.collection_native_text(expected[key][lang]))
    def test_only_known_source_values_are_keys(self):
        mapping=self.maps['item_notes']
        for text in ('Buscar...','[Defensa 999] Nombre del usuario','Nombre [fs] literal','[fs]Objeto que no existe',''):
            self.assertNotIn(text,mapping)
        self.assertTrue(all(a and b and a!=b for a,b in mapping.items()))
    def test_native_prefix_processing_and_runtime_color_conversion(self):
        self.assertEqual(annot.collection_native_text('[mp]Objeto sintético'),'Objeto sintético')
        self.assertEqual(annot.collection_native_text('[Artesanía: golpe triturador]\n[mp]Objeto sintético[normal]'),'[Artesanía: golpe triturador]\n[mp]Objeto sintético[normal]')
        mapping=annot.collection_presentation_pairs({'x':'[fs]Nombre oficial'},{'x':'ÿc5[Datos]\nÿc4[fs]Nombre oficial'})
        self.assertEqual(mapping['\ue07e5[Datos]\n\ue07e4[fs]Nombre oficial'],'Nombre oficial')
        self.assertNotIn('Nombre oficial',mapping)
    def test_generator_preserves_legitimate_internal_brackets(self):
        mapping=annot.collection_presentation_pairs({'x':'[fs]Nombre [literal]','y':'Nombre [fs] literal'},{'x':'[Datos]\n[fs]Nombre [literal]','y':'[Datos]\nNombre [fs] literal'})
        self.assertEqual(mapping['[Datos]\n[fs]Nombre [literal]'],'Nombre [literal]')
        self.assertEqual(mapping['[Datos]\nNombre [fs] literal'],'Nombre [fs] literal')
    def test_map_is_deterministic_and_rejects_ambiguous_names(self):
        with self.assertRaises(ValueError):annot.collection_presentation_pairs({'a':'Uno','b':'Dos'},{'a':'[Datos]','b':'[Datos]'})
        self.assertEqual(annot.collection_presentation_pairs({'x':'[fs]Objeto'},{'x':'[fs]Objeto'}),{'[fs]Objeto':'Objeto'})
if __name__=='__main__':unittest.main()
