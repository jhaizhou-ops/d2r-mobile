#!/usr/bin/env python3
"""原版技能图集 / HUD 小图 → TouchArt PNG，原版 Skills / Skilldesc → 纯 C 索引 dm_touch_native_art.c。构建时从玩家自己的游戏文件生成，不进仓库。
来源与指纹冻结在仓库里的 Sources/TouchArt/native_art_manifest.json（只有原版路径与哈希）：读包后先核原件 SHA-256 / MD5 与两张表的 SHA-256，
解码后核 PNG 的 SHA-256（解码只解块、不重绘像素）；对不上就报错（游戏版本不对）。
用法：generate.py <原版 RomFS（Update）>   产物生成在原位置、git 忽略（dm/ios/.gitignore）：Sources/TouchArt/dm_native_*.png、Sources/dm_touch_native_art.c
（.c 里按 Sources/ 写了相对 include，PNG 由 DmTouchOverlay.m / 工程 / 测试按 Sources/TouchArt 引用，所以不挪目录）
Original game skill atlas / HUD small images → TouchArt PNG, original game Skills / Skilldesc → pure C index dm_touch_native_art.c. Generated at build time from the player's own game files, not kept in the repository.
Sources and fingerprints are frozen in Sources/TouchArt/native_art_manifest.json in the repository (only original game paths and hashes): after reading the package, first verify the SHA-256 / MD5 of the original files and the SHA-256 of the two tables,
after decoding verify the SHA-256 of the PNGs (decoding only decodes blocks, it does not redraw pixels); on a mismatch report an error (wrong game version).
Usage: generate.py <original game RomFS (Update)>   The build output is generated in place and git-ignored (dm/ios/.gitignore): Sources/TouchArt/dm_native_*.png, Sources/dm_touch_native_art.c
(the .c has a relative include written against Sources/, and the PNGs are referenced by DmTouchOverlay.m / the project / the tests via Sources/TouchArt, so the directory is not moved)"""
import csv, hashlib, io, json, os, pathlib, sys
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


def main(romfs):
    mpath = IOS / 'Sources/TouchArt/native_art_manifest.json'; man = json.loads(mpath.read_text())
    tables = {}
    for t in man['official_tables']:
        raw = romfs_art.read(romfs, 'global/excel/' + t['file'])
        if sha(raw) != t['sha256']: raise SystemExit('native_art：游戏文件里的 %s 和冻结的不一样（游戏版本不对？要 3.3.94012）' % t['file'])
        tables[t['file']] = list(csv.DictReader(io.StringIO(raw.decode('utf-8')), delimiter='\t'))
    # PNG 落点：环境变量 DM_NATIVE_ART_DIR，缺省仍在 iOS 源码树原位（iOS 出包不设，行为不变）。
    # Android 出包设它，PNG 落到 dm/android/build/gen-assets/native_art/，不再写 iOS 源码树。
    # 那个 .c 索引仍写原位：它用相对 include（../../host/dm_input.h）且 iOS 与 Android 都编它，挪走要改两边的编译设置。
    # PNG output directory: environment variable DM_NATIVE_ART_DIR, still in place in the iOS source tree by default (the iOS package does not
    # set it, behaviour unchanged). The Android package sets it, and the PNGs land in dm/android/build/gen-assets/native_art/, no longer
    # writing into the iOS source tree. The .c index stays where it is: it includes relatively (../../host/dm_input.h) and both iOS and Android
    # compile it, so moving it would mean changing both build configurations.
    art = pathlib.Path(os.environ.get('DM_NATIVE_ART_DIR') or (IOS / 'Sources/TouchArt')); art.mkdir(parents=True, exist_ok=True); atlases = {}
    for a in man['assets']:
        raw = romfs_art.read(romfs, a['official_path'].split('data:data/', 1)[-1])
        if sha(raw) != a['official_sprite_sha256'] or hashlib.md5(raw).hexdigest() != a['official_sprite_md5']:
            raise SystemExit('native_art：%s 的原件和冻结的不一样（游戏版本不对？）' % a['official_path'])
        b = io.BytesIO(); romfs_art.sprite(romfs, a['official_path'].split('data:data/', 1)[-1]).save(b, 'PNG')
        if sha(b.getvalue()) != a['decoded_png_sha256']: raise SystemExit('native_art：%s 解出的 PNG 与冻结的不一致（Pillow 版本？）' % a['asset'])
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
    # .c 索引的落点：环境变量 DM_NATIVE_ART_C，缺省仍是 Sources/ 原位（iOS 出包不设，行为不变）。
    # Android 出包设它，产物落在它自己的构建树里，iOS 源码树一个字都不动。写去别处时那个相对 include
    # （../../host/dm_input.h，按 Sources/ 的深度写的）换成走 include 路径的写法——dm/host 在两边都在 include 路径里，结果逐字同义。
    # Output path of the .c index: environment variable DM_NATIVE_ART_C, still in place under Sources/ by default (the iOS package does not set
    # it, behaviour unchanged). The Android package sets it, so the product lands in its own build tree and the iOS source tree is not touched at
    # all. When written elsewhere, the relative include (../../host/dm_input.h, written for the depth of Sources/) is rewritten to the form that
    # goes through the include path -- dm/host is in the include path on both sides, so the result is exactly equivalent.
    c_env = os.environ.get('DM_NATIVE_ART_C'); cpath = pathlib.Path(c_env) if c_env else IOS / 'Sources/dm_touch_native_art.c'
    inc = '"dm_input.h"' if c_env else '"../../host/dm_input.h"'
    out = ['/* Generated from frozen official Skills/Skilldesc; do not hand-edit rows. */', '#include "dm_touch_native_art.h"', '#include ' + inc, '#include <stddef.h>',
           'const dm_ta_atlas dm_ta_atlases[DM_TA_ATLASES]={']
    for a in atl: out.append(' {"%s",%d,%d,%d,%d,%d},' % (a['asset'][:-4], *a['atlas_px'], a['frame_width'], a['frame_pitch'], a['frame_count']))
    out += ['};', f'const char dm_ta_manifest_sha[]="{sha(mpath.read_bytes())}";', f'const char dm_ta_geometry_sha[]="{sha(gpath.read_bytes())}";',
            f'const char dm_ta_binding_sha[]="{sha(binding.read_bytes())}";', 'static const struct { uint32_t id; int atlas,frame; } rows[]={']
    out += [' {%d,%d,%d},' % tuple(row) for row in rows]; out += ['};']
    out += [(HERE / 'implementation.c.inc').read_text()]
    (cpath.parent / cpath.name).write_text('\n'.join(out) + '\n')
    print(f'native_art：{len(rows)} 条技能索引、{len(atlases)} 张 PNG（原件 SHA256 + MD5、PNG SHA256 都核过）→ {art}；索引 .c → {cpath}')


if __name__ == '__main__':
    main(sys.argv[1])
