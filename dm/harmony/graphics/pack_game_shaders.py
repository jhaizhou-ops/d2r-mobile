"""Pack verified .vk.json/.spv into runtime XGS1 files (original bytes external)."""
import argparse, hashlib, json, pathlib, struct

TYPES = {'sampler':0,'combined_image_sampler':1,'sampled_image':2,'storage_image':3,
         'uniform_texel_buffer':4,'storage_texel_buffer':5,'uniform_buffer':6,'storage_buffer':7}
STAGES = {'vs':0,'hs':1,'ds':2,'gs':3,'ps':4,'cs':5}

def pack(path):
    j=json.loads(path.read_text(encoding='utf-8-sig')); key=j['identity']['key']
    spv=path.with_name(key+'.spv').read_bytes()
    if hashlib.sha256(spv).hexdigest()!=j['sha256']: raise ValueError('SPIR-V hash mismatch')
    bindings=[]
    for b in j['descriptors']:
        s=b['source']; n=s.get('nvn',{}); k=n.get('kind'); slot=n.get('slot',0)
        if s['category']=='support_buffer': source=1
        elif k in ('uniform','uniform_pointer'): source=2
        elif k=='storage_pointer': source=3
        elif k=='c1': source=4
        elif k=='driver' and n['bank'] in (0,2): source=5 if n['bank']==0 else 6
        elif k in ('texture','image') and slot is not None: source=7 if k=='texture' else 8
        else: raise ValueError(f'unmapped descriptor {b["name"]}: {s}')
        size=1392 if source==1 else b.get('block',{}).get('bytes',0)
        sampler=n.get('sampler_slot'); sampler=0xffffffff if sampler is None else sampler
        # Without an explicit sampler slot use the sampler in the combined NVN handle.
        flags=0
        if b['type'] in ('storage_image','storage_texel_buffer') and b.get('image',{}).get('format')==0:
            if 56 in j['capabilities']: flags|=1
            if 55 in j['capabilities']: flags|=2
        if 'image' in b:
            image=b['image'];scalar=image['scalar']
            kind=0 if scalar['kind']=='float' else 2 if scalar.get('signed') else 1
            flags|=(1<<31)|(image['dim']<<8)|(image['arrayed']<<12)|(image['multisampled']<<13)|(image['depth']==1)<<14|(kind<<16)
        if source in (7,8):
            hd=s['handle_decode']
            if hd['type'] not in (0,1): raise ValueError(f'unsupported handle type {hd}')
        bindings.append(struct.pack('<8I',b['binding'],TYPES[b['type']],b['count'],size,
                                    source,slot or 0,sampler,flags))
    ins=sorted({x['location'] for x in j['interfaces'] if x['storage']=='input' and x['location'] is not None}) if j['stage']=='vs' else []
    c1=bytes.fromhex(j['frozen_job'].get('c1',''))
    if len(c1)%4: raise ValueError('c1 alignment')
    header=struct.pack('<8I',0x31534758,1,STAGES[j['stage']],len(bindings),len(ins),len(c1),len(spv),0)
    return key,header+b''.join(bindings)+struct.pack('<%dI'%len(ins),*ins)+c1+spv

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--bundle',type=pathlib.Path,required=True)
    ap.add_argument('--output',type=pathlib.Path,required=True);args=ap.parse_args()
    if args.output.resolve().is_relative_to(pathlib.Path(__file__).resolve().parents[2]): raise ValueError('output must be outside repository')
    args.output.mkdir(parents=True,exist_ok=False); report=[]
    for p in sorted(args.bundle.glob('*.vk.json')):
        key,data=pack(p);(args.output/(key+'.xgs')).write_bytes(data)
        report.append({'key':key,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    (args.output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'packed {len(report)} genuine shaders')
if __name__=='__main__': main()
