#!/usr/bin/env python3
"""把纯源码模板和用户本地生成输入组装为仓外DevEco工程；不签名。
Assemble a source template and locally generated user inputs into an external unsigned DevEco project.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
from generate_experience import generate

ROOT = Path(__file__).resolve().parents[1]


def prepare(output, runtime, native_art, experience):
    """只组装新目录，拒绝覆写源树。 | Assemble only a new directory; refuse to overwrite the source tree."""
    output = output.resolve()
    if output.exists() or output.is_relative_to(ROOT.parents[1]) or not str(output).isascii():
        raise ValueError('output must be a new external ASCII directory')
    for path in [runtime, native_art, experience]:
        if not path.is_file():
            raise ValueError(f'missing local input: {path.name}')
    raw = runtime.read_bytes()
    if len(raw) < 64 or raw[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<HH', raw, 16) != (3, 183):
        raise ValueError('runtime must be an ELF64 little-endian AArch64 shared library')
    art = native_art.read_text(encoding='utf-8')
    if 'sw_ta_current_set' not in art or 'sw_ta_atlases' not in art:
        raise ValueError('generate native art with tools/generate_native_art.py first')
    exp = generate(experience)
    shutil.copytree(ROOT / 'app', output)
    cpp = output / 'entry/src/main/cpp'
    (cpp / 'touch-core/ios/Sources/sw_touch_native_art.c').write_text(art, encoding='utf-8')
    (output / 'entry/src/main/ets/data/Experience.generated.ts').write_text(exp, encoding='utf-8')
    libs = output / 'entry/libs/arm64-v8a'
    libs.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(runtime, libs / 'libxblr_execution.so')
    (cpp / 'real-shader.local.cmake').write_text(
        f'set(XBL_HARMONY_SOURCE "{ROOT.as_posix()}")\n'
        f'set(XBL_RUNTIME_INCLUDE "{ROOT.as_posix()}/runtime/include")\n', encoding='utf-8')
    (output / 'local-inputs.json').write_text(json.dumps({
        'runtime_sha256': hashlib.sha256(runtime.read_bytes()).hexdigest(),
        'native_art_sha256': hashlib.sha256(art.encode()).hexdigest(),
        'experience_sha256': hashlib.sha256(experience.read_bytes()).hexdigest(),
        'signing': 'NOT_CONFIGURED'}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--runtime', type=Path, required=True)
    ap.add_argument('--native-art-c', type=Path, required=True)
    ap.add_argument('--experience-txt', type=Path, required=True)
    args = ap.parse_args()
    prepare(args.output, args.runtime, args.native_art_c, args.experience_txt)
    print('External unsigned project assembled; compile, sign and device-test it locally.')
