"""Copy only missing original files to this debug app's incoming directory.

No delete, uninstall, or overwrite of the committed resource/save directories.
Application SHA256 validation commits incoming files. A durable local transfer
ledger avoids repeating completed large transfers after connection interruption.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser()
for name in ['hdc','device','source','manifest','state','evidence']:
    p.add_argument('--'+name,required=True)
a=p.parse_args();root=Path(a.source).resolve();e=Path(a.evidence);e.mkdir(parents=True,exist_ok=True)
manifest=json.loads(Path(a.manifest).read_text(encoding='utf-8-sig'))
state=json.loads(Path(a.state).read_text(encoding='utf-8-sig'))
expected='/data/storage/el2/base/haps/entry/files/game-data'
assert state['romfs']==expected and state['save']!=expected
ledger_path=e/'transfers.json';ledger=json.loads(ledger_path.read_text()) if ledger_path.exists() else {}
needed=[x for x in manifest if x['name'] in state['missing'] and ledger.get(x['name'])!=x['sha256']]
hdc=[a.hdc,'-t',a.device]
free=subprocess.run(hdc+['shell','df','-k','/data'],capture_output=True,text=True,check=True).stdout
(e/'device-space-before.txt').write_text(free)
available=int(free.strip().splitlines()[-1].split()[3])*1024
total=sum(x['bytes'] for x in needed)
assert available>=total+1024**3, 'Insufficient free space plus 1 GiB reserve'
print(f'pendingFiles={len(needed)} newBytes={total} availableBytes={available}',flush=True)
for i,item in enumerate(needed,1):
    name=item['name'];path=(root/name).resolve()
    assert path.parent==root and path.is_file() and path.stat().st_size==item['bytes']
    with path.open('rb') as f: assert hashlib.file_digest(f,'sha256').hexdigest()==item['sha256']
    print(f'{i}/{len(needed)} sending {name} bytes={item["bytes"]}',flush=True)
    dest='/data/storage/el2/base/haps/entry/files/incoming/'+name
    result=subprocess.run(hdc+['file','send','-b','com.xiaoboluo.harmony.bootstrap',str(path),dest],capture_output=True,text=True,timeout=1800)
    output=result.stdout+result.stderr
    (e/(name+'.transfer.txt')).write_text(output)
    if result.returncode or 'FileTransfer finish' not in output or '[Fail]' in output:
        raise RuntimeError(f'Transfer failed for {name}: {output}')
    ledger[name]=item['sha256'];temp=ledger_path.with_suffix('.new');temp.write_text(json.dumps(ledger,indent=2));temp.replace(ledger_path)
    print(output.strip(),flush=True)
print('TRANSFER_COMPLETE; run application validation before declaring data ready',flush=True)
