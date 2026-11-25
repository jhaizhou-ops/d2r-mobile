"""Verify bundled OHOS runtime against the actual compiled HAP/APP, not cache files."""
import argparse,hashlib,importlib.util,io,json,zipfile
from pathlib import Path

def check(hap, contract, expected):
    with zipfile.ZipFile(io.BytesIO(hap)) as z:
        lib=z.read('libs/arm64-v8a/libdm.so')
        if expected.encode('ascii')+b'\0' not in lib:
            raise ValueError('compiled libdm overlay fingerprint differs from source/runtime')
        marker=z.read('resources/resfile/dm/pack-version.txt').strip()
        if not marker: raise ValueError('empty runtime version marker')
        with zipfile.ZipFile(io.BytesIO(z.read('resources/resfile/dm/dm-pack.zip'))) as pack:
            entries=contract.parse_manifest(pack.read('overlay/overlay.manifest'),expected)
            names={n[len('overlay/'):] for n in pack.namelist() if n.startswith('overlay/') and not n.endswith('/') and n!='overlay/overlay.manifest'}
            if names!=set(entries): raise ValueError('overlay inventory differs from manifest')
            for n,digest in entries.items():
                if hashlib.sha256(pack.read('overlay/'+n)).hexdigest()!=digest: raise ValueError('overlay hash mismatch: '+n)
            for name in ('derived.tsv','pack_info.txt','out/main.patched.bin','out/sdk.patched.bin','out/main.fixups.tsv','out/sdk.fixups.tsv','out/main.gates.tsv','out/sdk.gates.tsv'):
                if not pack.read(name): raise ValueError('empty runtime member: '+name)
            if not any(n.startswith('shaders/') and n.endswith('.xgs') for n in pack.namelist()): raise ValueError('no shaders')
            if not any(n.startswith('ui-art/') and not n.endswith('/') for n in pack.namelist()): raise ValueError('no touch art')
        for name in ('main','sdk'):
            if not z.read('libs/arm64-v8a/libdm_guest_'+name+'.so'): raise ValueError('missing signed image')
    return {'fingerprint':expected,'overlay_files_verified':len(entries),'compiled_contract_matches':True}

def main():
    p=argparse.ArgumentParser();p.add_argument('--repo-root',type=Path,required=True);p.add_argument('package',type=Path);a=p.parse_args()
    spec=importlib.util.spec_from_file_location('overlay_contract',a.repo_root/'dm/ios/tools/overlay_contract.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    expected=m.source_fingerprint(a.repo_root)
    with zipfile.ZipFile(a.package) as z:
        modules=[n for n in z.namelist() if n.endswith('.hap')]
        results=[check(z.read(n),m,expected) for n in modules] if modules else [check(a.package.read_bytes(),m,expected)]
    print(json.dumps({'package':str(a.package),'checks':results},indent=2))
if __name__=='__main__': main()
