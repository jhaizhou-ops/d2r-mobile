"""Stage the complete private runtime ZIP for default/selfuse, never the public shell."""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile
import zipfile

DIRECTORIES = ('out', 'shaders', 'overlay', 'ui-art')
METADATA = ('derived.tsv', 'pack_info.txt')

def stage(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    for name in DIRECTORIES:
        if not (source / name).is_dir():
            raise ValueError('missing directory: ' + name)
    for name in METADATA:
        if not (source / name).is_file() or (source / name).stat().st_size == 0:
            raise ValueError('missing or empty metadata: ' + name)
    info = dict(line.split('=', 1) for line in (source / 'pack_info.txt').read_text(encoding='utf-8').splitlines() if '=' in line)
    if info.get('TARGET') != 'ohos' or not info.get('PRODUCT'):
        raise ValueError('pack_info.txt must identify an ohos runtime pack')
    if destination.is_relative_to(source):
        raise ValueError('destination must be outside the runtime input')
    files = [source / name for name in METADATA]
    for name in DIRECTORIES:
        files.extend(p for p in (source / name).rglob('*') if p.is_file())
    files.sort(key=lambda p: p.relative_to(source).as_posix())
    destination.mkdir(parents=True, exist_ok=True)
    version = hashlib.sha256()
    fd, temporary_name = tempfile.mkstemp(prefix='dm-pack-', suffix='.tmp', dir=destination)
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in files:
                if path.is_symlink() or not path.resolve().is_relative_to(source):
                    raise ValueError('runtime input escapes its source: ' + str(path))
                relative = path.relative_to(source).as_posix()
                content = path.read_bytes()
                version.update(relative.encode('utf-8') + b'\0' + hashlib.sha256(content).digest())
                archive.writestr(relative, content)
        # Complete the ZIP before publishing the new version marker.
        os.replace(temporary, destination / 'dm-pack.zip')
        marker = destination / 'pack-version.txt.tmp'
        marker.write_text(version.hexdigest() + '\n', encoding='utf-8')
        os.replace(marker, destination / 'pack-version.txt')
    finally:
        if temporary.exists():
            temporary.unlink()
    return dict(files=len(files), bytes=(destination / 'dm-pack.zip').stat().st_size, version=version.hexdigest())

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('--dest', default=str(Path(__file__).resolve().parents[1] / 'entry/src/debugpack/resources/resfile/dm'))
    args = parser.parse_args()
    print(stage(args.source, args.dest))
