"""Validate and deploy generated XGS files to the debug app, with restart ledger."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser()
for name in ('source', 'evidence', 'hdc', 'device'):
    p.add_argument('--' + name, required=True)
a = p.parse_args()
root = Path(a.source).resolve()
evidence = Path(a.evidence).resolve()
evidence.mkdir(parents=True, exist_ok=True)
manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8-sig'))
assert len(manifest) == 6927
ledger_path = evidence / 'transferred.json'
ledger = json.loads(ledger_path.read_text()) if ledger_path.exists() else {}
hdc = [a.hdc, '-t', a.device]
started = time.monotonic()
sent = 0
with (evidence / 'transfer.log').open('a', encoding='utf-8') as log:
    for i, item in enumerate(manifest):
        key = item['key']
        assert len(key) == 16 and all(c in '0123456789abcdef' for c in key)
        source = root / (key + '.xgs')
        assert source.stat().st_size == item['bytes']
        with source.open('rb') as f:
            assert hashlib.file_digest(f, 'sha256').hexdigest() == item['sha256']
        if ledger.get(key) == item['sha256']:
            continue
        destination = '/data/storage/el2/base/haps/entry/files/game-shaders/' + source.name
        r = subprocess.run(hdc + ['file', 'send', '-b', 'com.xiaoboluo.harmony.bootstrap', str(source), destination],
                           capture_output=True, text=True, timeout=120)
        output = r.stdout + r.stderr
        log.write(key + '\n' + output)
        log.flush()
        if r.returncode or '[Fail]' in output or 'FileTransfer finish' not in output:
            raise RuntimeError(f'{key}: {output}')
        ledger[key] = item['sha256']
        pending = ledger_path.with_suffix('.new')
        pending.write_text(json.dumps(ledger), encoding='utf-8')
        pending.replace(ledger_path)
        sent += 1
        if sent % 100 == 0:
            print(f'transferred={len(ledger)}/6927 elapsed={time.monotonic()-started:.1f}s', flush=True)
print(f'COMPLETE validated=6927 transferredNow={sent} totalBytes={sum(x["bytes"] for x in manifest)}', flush=True)
