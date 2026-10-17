#!/usr/bin/env python3
"""模块清单：assets/module-manifest.json + assets/modules/*（打包用；来源 = 本树改写器产出的模块目录）。"""
import hashlib
import json
import os
import shutil
import sys

moddir, assets = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(assets, 'modules'), exist_ok=True)
files = []
for n in sorted(os.listdir(moddir)):
    if not n.endswith(('.patched.bin', '.fixups.tsv', '.gates.tsv')):
        continue
    p = os.path.join(moddir, n)
    h = hashlib.sha256(open(p, 'rb').read()).hexdigest()
    files.append({'name': n, 'bytes': os.path.getsize(p), 'sha256': h})
    shutil.copy2(p, os.path.join(assets, 'modules', n))
json.dump({'files': files}, open(os.path.join(assets, 'module-manifest.json'), 'w'), indent=1)
print('module-manifest.json:', [(f['name'], f['bytes']) for f in files])
