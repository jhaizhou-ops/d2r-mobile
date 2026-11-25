from pathlib import Path
import struct, sys, tempfile, unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from d2rm_warm_seed import load_seed, stage_seed

def record(key):
    return struct.pack('<III',0x57504C58,1,len(key))+key+struct.pack('<II',1,7)

class WarmSeedTests(unittest.TestCase):
    def test_valid_and_copy_exactly_validated_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            path=folder/'input.bin'
            data=record(b'alpha')+record(b'beta')
            path.write_bytes(data)
            seed=load_seed(path)
            self.assertEqual(seed[1:],(2,{'alpha','beta'}))
            (folder/'alpha.xgs').touch()
            with self.assertRaises(ValueError): stage_seed(seed,folder)
            self.assertFalse((folder/'vk-pipeline-list.seed').exists())
            (folder/'beta.xgs').touch()
            path.write_bytes(b'changed since validation')
            self.assertEqual(len(stage_seed(seed,folder)),64)
            self.assertEqual((folder/'vk-pipeline-list.seed').read_bytes(),data)

    def test_reject_invalid_before_staging(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'input.bin'
            good=record(b'x')
            for sample in (b'',b'bad'+good,good[:-1],good+b'x',record(b'../x'),
                           record(b''),record(b'\xff'),record(b'a'*257),
                           good[:4]+struct.pack('<I',6)+good[8:],
                           good[:-8]+struct.pack('<II',4097,1)):
                with self.subTest(sample=sample):
                    path.write_bytes(sample)
                    with self.assertRaises(ValueError):load_seed(path)

if __name__=='__main__':
    unittest.main()
