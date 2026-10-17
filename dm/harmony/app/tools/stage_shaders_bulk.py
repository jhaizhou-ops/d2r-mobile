"""Resume verified XGS deployment using one HDC directory transfer."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
for key in ('source','evidence','hdc','device'): p.add_argument('--'+key,required=True)
a=p.parse_args();root=Path(a.source).resolve();e=Path(a.evidence).resolve()
manifest=json.loads((root/'manifest.json').read_text(encoding='utf-8-sig'))
ledger=json.loads((e/'transferred.json').read_text())
stage=e/'bulk'/'game-shaders';stage.mkdir(parents=True,exist_ok=True)
pending=[]
for item in manifest:
    key=item['key'];assert len(key)==16 and all(c in '0123456789abcdef' for c in key)
    source=root/(key+'.xgs');assert source.stat().st_size==item['bytes']
    with source.open('rb') as f: assert hashlib.file_digest(f,'sha256').hexdigest()==item['sha256']
    if ledger.get(key)==item['sha256']:continue
    pending.append(item);target=stage/source.name
    if not target.exists():os.link(source,target)
print(f'validated={len(manifest)} retained={len(ledger)} bulkRemaining={len(pending)} bytes={sum(x["bytes"] for x in pending)}',flush=True)
(e/'bulk-manifest.json').write_text(json.dumps(pending),encoding='utf-8')
# HDC puts the source directory's basename under an existing destination.
r=subprocess.run([a.hdc,'-t',a.device,'file','send','-b','com.xiaoboluo.harmony.bootstrap',str(stage),'/data/storage/el2/base/haps/entry/files'],capture_output=True,text=True,timeout=1800)
output=r.stdout+r.stderr;(e/'bulk-transfer.log').write_text(output,encoding='utf-8');print(output,flush=True)
assert r.returncode==0 and '[Fail]' not in output and 'FileTransfer finish' in output
for item in pending:ledger[item['key']]=item['sha256']
temp=e/'transferred.new';temp.write_text(json.dumps(ledger),encoding='utf-8');temp.replace(e/'transferred.json')
print('COMPLETE '+str(len(ledger)),flush=True)
