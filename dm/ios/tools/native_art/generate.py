#!/usr/bin/env python3
"""原版技能图集 / HUD 小图 → TouchArt PNG，原版 Skills / Skilldesc → 纯 C 索引 dm_touch_native_art.c。构建时从玩家自己的游戏文件生成，不进仓库。
来源与指纹冻结在仓库里的 Sources/TouchArt/native_art_manifest.json（只有原版路径与哈希）：读包后先核原件 SHA-256 / MD5 与两张表的 SHA-256，
解码后核尺寸与 RGBA 像素 SHA-256（不依赖 PNG 压缩编码）；对不上仍报错。
用法：generate.py <原版 RomFS（Update）> [--output-dir 仓外目录]。默认生成在被git忽略的Sources/；指定仓外目录则生成dm_touch_native_art.c及TouchArt/。
安卓编译时给索引添加公共头搜索路径，PNG仅作为外部overlay导入，不进安装包。
Original game skill atlas / HUD small images → TouchArt PNG, original game Skills / Skilldesc → pure C index dm_touch_native_art.c. Generated at build time from the player's own game files, not kept in the repository.
Sources and fingerprints are frozen in Sources/TouchArt/native_art_manifest.json in the repository (only original game paths and hashes): after reading the package, first verify the SHA-256 / MD5 of the original files and the SHA-256 of the two tables,
after decoding verify dimensions and RGBA pixel SHA-256, independently of PNG compression; a mismatch remains fatal.
Usage: generate.py <original game RomFS (Update)> [--output-dir external-directory]. The default remains git-ignored Sources/; an external directory receives dm_touch_native_art.c and TouchArt/.
Android adds public header search paths for the index; PNGs are imported only as an external overlay and never packaged in the APK."""
import argparse, csv, hashlib, io, json, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent; IOS = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))
import romfs_art
sha = lambda b: hashlib.sha256(b).hexdigest()
canonical = lambda obj: json.dumps(obj, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()
classes = ['amazon', 'Sorceress', 'Necromancer', 'paladin', 'Barbarian', 'Druid', 'Assassin', 'Warlock', 'SubMenu']
codes = ['ama', 'sor', 'nec', 'pal', 'bar', 'dru', 'ass', 'war', '']
classes_by_code = {v: i for i, v in enumerate(codes)}
COMMON_ICON = {357: 20, 358: 22, 360: 24, 361: 30, 362: 32, 363: 34, 364: 36, 370: 38}


def validate_rgba(image, asset):
    """冻结的是像素，PNG 压缩器可以不同。 | Freeze pixels, not a PNG compressor's bytes. | rgba = image.convert('RGBA') if rgba.size != tuple(asset['atlas_px']):"""
    rgba = image.convert('RGBA')
    if rgba.size != tuple(asset['atlas_px']):
        raise SystemExit('native_art：%s 解出的尺寸与冻结的不一致' % asset['asset'])
    if sha(rgba.tobytes()) != asset['decoded_rgba_sha256']:
        raise SystemExit('native_art：%s 解出的 RGBA 像素与冻结的不一致' % asset['asset'])
    return rgba


def main(romfs, output=None):
    mpath = IOS / 'Sources/TouchArt/native_art_manifest.json'; man = json.loads(mpath.read_text())
    tables = {}
    for t in man['official_tables']:
        raw = romfs_art.read(romfs, 'global/excel/' + t['file'])
        if sha(raw) != t['sha256']: raise SystemExit('native_art：游戏文件里的 %s 和冻结的不一样（游戏版本不对？要 3.3.94012）' % t['file'])
        tables[t['file']] = list(csv.DictReader(io.StringIO(raw.decode('utf-8')), delimiter='\t'))
    # 安卓可把所有生成物放仓外；苹果默认目录保持原样。
    # Android can keep every generated file outside the checkout; Apple retains its default directories.
    destination = Path(output) if output else IOS / 'Sources'
    art = destination / 'TouchArt'; art.mkdir(parents=True, exist_ok=True); atlases = {}
    for a in man['assets']:
        raw = romfs_art.read(romfs, a['official_path'].split('data:data/', 1)[-1])
        if sha(raw) != a['official_sprite_sha256'] or hashlib.md5(raw).hexdigest() != a['official_sprite_md5']:
            raise SystemExit('native_art：%s 的原件和冻结的不一样（游戏版本不对？）' % a['official_path'])
        image = validate_rgba(romfs_art.sprite(romfs, a['official_path'].split('data:data/', 1)[-1]), a)
        b = io.BytesIO(); image.save(b, 'PNG')
        (art / a['asset']).write_bytes(b.getvalue()); atlases[a['asset']] = a
    atl = [atlases['dm_native_skill_%d.png' % i] for i in range(len(classes))]
    desc = {r['skilldesc']: r for r in tables['skilldesc.txt']}; rows = []
    for r in tables['skills.txt']:
        raw = int(r['*Id']); char = classes_by_code.get(r['charclass'], -1); cel = desc.get(r['skilldesc'], {}).get('IconCel', '')
        frame = int(cel) if cel.strip() else -1
        if char < 0 or frame < 0 or frame + 1 >= atl[char]['frame_count']: char = frame = -1
        # 手柄版后加的通用动作：官方表的 IconCel 是占位值 2（普通攻击那一帧），游戏的技能树 / 绑定框画的是通用图集里各自的图。
        # 帧号逐个对过原版绑定框里同一技能的大图标（综合技能页逐个长按）：交互、拾取、表情、换武器、地图、显示物品、跑 / 走、方块拾取
        # Generic actions added later by the controller version: IconCel in the official table is the placeholder value 2 (the normal attack frame), while the game's skill tree / binding box draws each one's own image from the generic atlas.
        # The frame numbers were checked one by one against the big icon of the same skill in the original game's binding box (long-pressing each on the General Skills page): interact, pick up, emote, swap weapons, map, show items, run / walk, block pickup
        if raw in COMMON_ICON: frame = COMMON_ICON[raw]; assert char >= 0 and frame + 1 < atl[char]['frame_count']
        rows.append([raw, char, frame])
    assert len({r[0] for r in rows}) == len(rows)
    if sha(canonical(rows)) != man['descriptor_index_sha256']: raise SystemExit('native_art：技能索引和冻结的不一致')
    gpath = IOS / 'docs/native_art_v1_geometry.json'; binding = IOS / 'docs/touch_layout_spec_native_binding_v5.json'
    out = ['/* 从冻结原版表生成，不手改。 | Generated from frozen official tables; do not hand-edit. */', '#include "dm_touch_native_art.h"', '#include "dm_input.h"' if output else '#include "../../host/dm_input.h"', '#include <stddef.h>',
           'const dm_ta_atlas dm_ta_atlases[DM_TA_ATLASES]={']
    for a in atl: out.append(' {"%s",%d,%d,%d,%d,%d},' % (a['asset'][:-4], *a['atlas_px'], a['frame_width'], a['frame_pitch'], a['frame_count']))
    out += ['};', f'const char dm_ta_manifest_sha[]="{sha(mpath.read_bytes())}";', f'const char dm_ta_geometry_sha[]="{sha(gpath.read_bytes())}";',
            f'const char dm_ta_binding_sha[]="{sha(binding.read_bytes())}";', 'static const struct { uint32_t id; int atlas,frame; } rows[]={']
    out += [' {%d,%d,%d},' % tuple(row) for row in rows]; out += ['};']
    out += [(HERE / 'implementation.c.inc').read_text()]
    with (destination / 'dm_touch_native_art.c').open('w', encoding='utf-8', newline='\n') as f:
      f.write('\n'.join(out) + '\n')
    print(f'native_art：{len(rows)} 条技能索引、{len(atlases)} 张 PNG（原件 SHA256 + MD5、尺寸与 RGBA SHA256 都核过）→ {art}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate verified native art from your own RomFS')
    parser.add_argument('romfs')
    parser.add_argument('--output-dir', help='External generated-source directory; contains TouchArt/')
    args = parser.parse_args()
    main(args.romfs, args.output_dir)
