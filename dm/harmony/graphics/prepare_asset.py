"""Extract explicitly selected original resources read-only with the existing audited RomFS reader.
No original or derived resource is written into a repository; output must be new.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--romfs-reader', type=Path, required=True)
    p.add_argument('--path', action='append', required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    root = a.root.resolve(strict=True)
    output = a.output.resolve()
    if output.is_relative_to(root) or output.exists():
        raise ValueError('output must be a new directory outside source')
    spec = importlib.util.spec_from_file_location('xbg_romfs_reader', a.romfs_reader)
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    reader.check(root)
    wanted = set(a.path)
    rows = []
    with (root / 'build_manifest.psv').open(encoding='utf-8') as f:
        for line in f:
            row = line.rstrip('\r\n').split('|')
            if len(row) == 11 and row[9] in wanted:
                rows.append(row)
    if len(rows) != len(wanted) or {row[9] for row in rows} != wanted:
        raise ValueError('requested paths not unique/present in manifest')
    output.mkdir(parents=True)
    report = {'source':str(root), 'reader_sha256':hashlib.sha256(a.romfs_reader.read_bytes()).hexdigest(), 'files':[]}
    for index,row in enumerate(rows):
        key=bytes.fromhex(row[6]); chunk,arch=int(row[2]),int(row[3])
        if key[9]!=chunk or key[10]!=arch: raise ValueError('EKey archive mismatch')
        archive=f'{chunk:02d}-{arch:08d}.data'; offset=int.from_bytes(key[11:16],'big')
        raw=reader.Archive(root/archive).read(offset,int(row[7]))
        data=reader.decode(raw,row[8],int(row[5]),row[4])
        name=f'{index:02d}'+Path(row[9]).suffix
        (output/name).write_bytes(data)
        report['files'].append({'path':row[9],'filename':name,'ckey':row[4], 'ekey':row[6],
            'archive':archive,'logical_offset':offset,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    (output/'manifest.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False,indent=2))

if __name__=='__main__':main()
