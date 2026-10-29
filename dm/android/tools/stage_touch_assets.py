#!/usr/bin/env python3
"""把仓外验证过的技能图集与HUD加入安卓overlay，不改原资源或安装包。
Stage externally generated skill atlases and HUD into Android's overlay, leaving original resources and APKs alone.
"""
import argparse
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'ios/tools'))
import overlay_contract


def stage(native_art, hud, overlay):
    """只接收生成器的固定PNG及HUD引用，不接收源码、存档或签名文件。
    Accept generator PNGs and HUD references only, never source code, saves or signing files.
    """
    native_art, hud, overlay = map(Path, (native_art, hud, overlay))
    required = {f'dm_native_skill_{i}.png' for i in range(9)}
    required.update(('dm_native_map.png', 'dm_native_menu.png', 'dm_native_circle.png'))
    for name in sorted(required):
        path = native_art / name
        if not path.is_file() or path.is_symlink():
            raise ValueError(f'missing or unsafe native art: {name}')
    ui = json.loads((hud / 'ui.json').read_text(encoding='utf-8'))
    files = {'ui.json', 'base_full.png', 'lvl_0.png', 'lvl_1.png', 'lvl_d0.png', 'lvl_d1.png', 'slot.png'}
    files.update(f'diff_{i}.png' for i in range(1, 9))
    # 技能钮上画的是同帧位的银色剪影（gen_hud_v4.py 出，iOS 同一套），不是原版方形图集
    # The skill buttons draw the silver silhouettes at the same frame positions (from gen_hud_v4.py, as on iOS), not the square original atlases
    files.update(f'sil_dm_native_skill_{i}.png' for i in range(9))
    for group in ('lv', 'badge', 'vital'):
        for glyph in ui[group]['glyphs'].values():
            if glyph.get('file') is None and isinstance(glyph.get('adv'), (int, float)) and glyph['adv'] >= 0:
                continue
            name = overlay_contract.valid_relative_path(glyph['file'])
            if '/' in name or not name.endswith('.png'):
                raise ValueError('HUD glyph must be a flat PNG path')
            files.add(name)
    for name in sorted(files):
        path = hud / name
        if not path.is_file() or path.is_symlink():
            raise ValueError(f'missing or unsafe HUD asset: {name}')
    # 检查完整后才复制；最终manifest涵盖全部生成图像，启动时逐字节验证。
    # Copy only after completeness checks; the final manifest covers every generated image for byte verification at startup.
    for source, subdir, names in ((native_art, 'touchart', required), (hud, 'hud_v4_ui', files)):
        target = overlay / subdir
        target.mkdir(parents=True, exist_ok=True)
        for name in sorted(names):
            shutil.copyfile(source / name, target / name)
    # 技能钮底座（仓库自带的项目美术，不是游戏资源；iOS 同一张 ControlArt/dm_mobile_socket.png）一并放进 touchart，manifest 覆盖它。
    # The skill-button socket (project art shipped in the repository, not game art; the same ControlArt/dm_mobile_socket.png as iOS) goes
    # into touchart too, so the manifest covers it.
    socket = Path(__file__).resolve().parents[3] / 'dm/ios/Sources/ControlArt/dm_mobile_socket.png'
    if not socket.is_file() or socket.is_symlink():
        raise ValueError('missing or unsafe socket art: dm_mobile_socket.png')
    shutil.copyfile(socket, overlay / 'touchart' / 'dm_mobile_socket.png')
    source_sha = overlay_contract.source_fingerprint(Path(__file__).resolve().parents[3])
    overlay_contract.write_manifest(overlay, source_sha)
    entries = overlay_contract.verify_manifest(overlay, source_sha)
    print(json.dumps({'source_sha256': source_sha, 'overlay_files': len(entries), 'native_art': len(required), 'hud': len(files)}, sort_keys=True))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-art', required=True, type=Path, help='Generated TouchArt directory')
    parser.add_argument('--hud', required=True, type=Path, help='Generated hud_v4_ui directory')
    parser.add_argument('--overlay', required=True, type=Path, help='New candidate overlay, never a live imported directory')
    args = parser.parse_args()
    stage(args.native_art, args.hud, args.overlay)
