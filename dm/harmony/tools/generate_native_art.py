#!/usr/bin/env python3
"""复用上游原版素材生成器，在仓外生成sw前缀的技能接口文件。
Reuse the upstream original-art generator to produce an sw-prefixed skill interface outside Git.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
IOS = ROOT.parent / 'ios'


def generate(romfs, output):
    """复用原版指纹核验，不提供图集或技能表。 | Reuse original fingerprint verification; supply no atlases or skill tables."""
    output = output.resolve()
    if output.exists() or output.is_relative_to(ROOT.parents[1]):
        raise ValueError('output must be a new directory outside the checkout')
    source = IOS / 'tools/native_art/generate.py'
    output.mkdir(parents=True)
    art = output / 'Sources/TouchArt'
    art.mkdir(parents=True)
    for relative in ['Sources/TouchArt/native_art_manifest.json',
                     'docs/native_art_v1_geometry.json', 'docs/touch_layout_spec_native_binding_v5.json']:
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(IOS / relative, target)
    sys.path.insert(0, str(IOS / 'tools'))
    spec = importlib.util.spec_from_file_location('harmony_local_native_art', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.IOS = output
    module.main(str(romfs))
    # 只替换符号前缀；旧sw触控结构单独保留。 | Rename symbols only; keep the old sw touch structs isolated.
    generated = output / 'Sources/dm_touch_native_art.c'
    text = generated.read_text(encoding='utf-8').replace('dm_', 'sw_').replace('DM_', 'SW_')
    text = text[:text.index('static sw_ta_glyph unknown')] + (ROOT / 'tools/native_art_impl.c.inc').read_text(encoding='utf-8')
    result = output / 'sw_touch_native_art.c'
    result.write_text(text, encoding='utf-8')
    # 游戏图像仍在仓外，只为旧前端的资源名提供对应副本。 | Images stay outside Git; supply aliases expected by the older frontend.
    for path in art.glob('dm_native_*.png'):
        shutil.copyfile(path, art / path.name.replace('dm_', 'sw_', 1))
    print('Local native-art interface:', hashlib.sha256(result.read_bytes()).hexdigest())


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    generate(args.romfs, args.output)
