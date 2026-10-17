#!/usr/bin/env python3
"""由自有r32 NSO在仓外准备OHOS映像；不覆盖受控HLE源码。
Prepare OHOS images outside the checkout from owned r32 NSOs; never overwrite maintained HLE sources.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime/tools'))
from prepare_originals import decode, PINNED


def prepare(main, sdk, output):
    """先核原件与各段SHA，再生成外部文件。 | Verify original and segment hashes before generating external files."""
    if not __debug__:
        raise ValueError('do not use python -O: the historical rewriter requires assertion guards')
    output = output.resolve()
    if output.exists() or output.is_relative_to(ROOT.parents[1]) or not str(output).isascii():
        raise ValueError('output must be a new external ASCII directory')
    images = []
    for name, path in [('main', main), ('sdk', sdk)]:
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != PINNED[name]:
            raise ValueError(f'{name}: expected the pinned 3.3.94012-r32 original NSO')
        image, meta = decode(path)
        meta['name'] = name
        images.append((name, image, meta))
    output.mkdir(parents=True)
    originals = output / 'originals'
    originals.mkdir()
    for name, image, meta in images:
        (originals / f'{name}.original.image.bin').write_bytes(image)
    (originals / 'originals-manifest.json').write_text(json.dumps({
        'result': 'PASS', 'product': '3.3.94012-r32',
        'modules': [m for _, _, m in images]}, indent=2), encoding='utf-8')
    from rewrite_harmony import generate
    generated = output / 'generated'
    generate(originals, generated)
    from patch_main import apply
    apply(generated)
    sources = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(ROOT.rglob('*')) if p.is_file() and not set(p.parts).intersection({'__pycache__', '.evidence', 'build', 'oh_modules'})}
    (output / 'source-manifest.json').write_text(json.dumps(sources, indent=2), encoding='utf-8')
    return generated


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--main', required=True, type=Path)
    ap.add_argument('--sdk', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--native-sdk', type=Path)
    ap.add_argument('--opus-library', type=Path)
    ap.add_argument('--opus-include', type=Path)
    args = ap.parse_args()
    if args.native_sdk and not (args.opus_library and args.opus_include):
        ap.error('--native-sdk also requires --opus-library and --opus-include')
    generated = prepare(args.main, args.sdk, args.output)
    if args.native_sdk:
        sdk = args.native_sdk.resolve()
        suffix = '.exe' if sys.platform == 'win32' else ''
        cmake = sdk / f'build-tools/cmake/bin/cmake{suffix}'
        subprocess.run([str(cmake), '-S', str(ROOT / 'runtime'), '-B', str(args.output / 'build'),
                        '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={sdk}/build-tools/cmake/bin/ninja{suffix}',
                        f'-DCMAKE_TOOLCHAIN_FILE={sdk}/build/cmake/ohos.toolchain.cmake',
                        '-DOHOS_ARCH=arm64-v8a', '-DOHOS_COMPATIBLE_SDK_VERSION=26.0.0',
                        '-DOHOS_STL=none', '-DCMAKE_BUILD_TYPE=Release',
                        f'-DXBLR_GENERATED_EXECUTION={generated}',
                        f'-DXBLR_OPUS_LIBRARY={args.opus_library.resolve()}',
                        f'-DXBLR_OPUS_INCLUDE={args.opus_include.resolve()}'], check=True)
        subprocess.run([str(cmake), '--build', str(args.output / 'build'),
                        '--target', 'xblr_execution', '--parallel', '1'], check=True)
    print('External inputs prepared; no device execution or equivalence test was performed.')


if __name__ == '__main__':
    main()
