"""Use actual ELF HLE entries and the already-compiled identical host loader."""
import argparse,hashlib,json,struct,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from build_execution import elf_sections

def main(elf,originals,host_build,out):
    root=Path(__file__).resolve().parents[1]
    manifest=json.loads((host_build/'source-manifest.json').read_text())
    expected=next(r['sha256'] for r in manifest if r['path']=='src/loader_ohos.c')
    assert hashlib.sha256((root/'src/loader_ohos.c').read_bytes()).hexdigest()==expected,'host loader source differs'
    raw=elf.read_bytes();ph,sections,sh=elf_sections(raw);rel={}
    for sec in sh:
        if sec[1]==4:
            for p in range(sec[4],sec[4]+sec[5],24):
                a,info,v=struct.unpack_from('<QQq',raw,p)
                if info&0xffffffff==1027:rel[a]=v
    def file_offset(a):
        return next(s[4]+a-s[3] for s in sh if s[1]!=8 and s[3]<=a<s[3]+s[5])
    s=sections['xblr_hle'];assert s[5]%24==0;bindings=[]
    for off in range(0,s[5],24):
        address=s[3]+off;p=file_offset(rel[address]);name=raw[p:raw.index(0,p)].decode('ascii');fn=rel[address+8]
        force=struct.unpack_from('<I',raw,s[4]+off+16)[0]
        assert force in (0,1) and any(p[0]==1 and p[1]&1 and p[3]<=fn<p[3]+p[6] for p in ph)
        bindings.append({'name':name,'address':fn,'force':force})
    assert len({x['name'] for x in bindings})==len(bindings)
    out.mkdir(parents=True,exist_ok=False);(out/'hle-bindings.json').write_text(json.dumps(bindings,indent=2))
    run=subprocess.run([sys.executable,'-B',str(root/'tools/check_original_binding.py'),str(host_build/'loader_test.dll'),str(originals),str(out/'binding-result.json'),str(out/'hle-bindings.json')],capture_output=True,text=True,encoding='utf8',errors='replace')
    (out/'binding.log').write_text(run.stdout+run.stderr,encoding='utf8');assert run.returncode==0,run.stderr
    r=json.loads((out/'binding-result.json').read_text());summary={'result':'PASS','elf_sha256':hashlib.sha256(raw).hexdigest(),'compiled_hle_entries':len(bindings),'host_loader_source_sha256':expected,'host_loader_dll_sha256':hashlib.sha256((host_build/'loader_test.dll').read_bytes()).hexdigest(),'link':r['with_compiled_hle']['report'],'scope':'actual compiled HLE addresses planned by the same C loader; no guest/HLE execution'}
    (out/'result.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('elf',type=Path);p.add_argument('originals',type=Path);p.add_argument('host_build',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();main(a.elf,a.originals,a.host_build,a.output)
