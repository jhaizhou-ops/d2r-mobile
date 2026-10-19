#!/usr/bin/env python3
# 高价值掉落的光（第二批）：覆盖层里给物品的「地面模型定义」json 出两档带光变体，
# 由宿主在「地上物品取模型路径」那一刻按规则改指过去（dm/host/dm_dropfilter.c，剪口 svc #0x87 落在 main+0x8fa590）。
# 变体常驻覆盖层（不进开关组合份数）：开关、档位、筛选都由宿主运行时决定，没被改指的变体游戏不会去读。
# 变体 = 原模型定义 + 一个原版特效实体：一档 = 原版赫拉迪克光（白色竖光柱），二档 = 原版迪亚波罗光柱（橙色斜光束）。
# 不加贴图、不加着色器、不放大模型、不加爆闪 / 头顶感叹号。路径：data/hd/items/<类别>/zswg/<档>__<模型名，/ 换成 __>.json（新增路径，casc_overlay「+」）。
# 覆盖哪些模型：items.json / uniques.json / sets.json 指向的、在武器 / 盔甲两类下的全部模型（暗金 / 套装可以是任何底材），
# 加上杂项里会发光的几类（符文、戒指、项链、护身符、钥匙、任务道具）。原版表里指向包里不存在模型的条目跳过（宿主改指前不核存在性：
# 那些条目原本就画不出来）。
# Light on high-value drops (second batch): in the overlay, produce two tiers of lit variants of the items' "ground model definition" json,
# and the host redirects to them by rule at the moment of "ground item fetches its model path" (dm/host/dm_dropfilter.c, cut svc #0x87 lands at main+0x8fa590).
# Variants live permanently in the overlay (not counted in the number of switch-combination copies): switch, tier and filter are all decided by the host at run time, and the game never reads a variant that is not redirected to.
# Variant = original model definition + one effect entity from the original game: tier 1 = the original game's Horadric light (white vertical light shaft), tier 2 = the original game's Diablo light shaft (orange slanted beam).
# No textures added, no shaders added, no model enlargement, no flash burst / overhead exclamation mark. Path: data/hd/items/<category>/zswg/<tier>__<model name, / replaced by __>.json (new path, casc_overlay "+").
# Which models are covered: all models pointed to by items.json / uniques.json / sets.json under the weapon / armor categories (uniques / sets can be any base),
# plus the few glowing kinds among misc (runes, rings, amulets, charms, keys, quest items). Entries in the original tables that point to models missing from the package are skipped (the host does not check existence before redirecting:
# those entries could not be drawn in the first place).
import hashlib, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import touch_layout

GLOW = 'data/hd/vfx/particles/overlays/object/horadric_light/fx_horadric_light.particles'      # 一档 | Tier 1
SHAFT = 'data/hd/vfx/particles/missiles/diablo_light_shaft/fx_diablo_light_shaft.particles'     # 二档 | Tier 2
TIERS = {'1': GLOW, '2': SHAFT}
ROOTS = ('weapon', 'armor', 'misc')
MISC_LIT = ('rune/', 'ring/', 'amulet/', 'charm/', 'key/', 'quest/')

def add_light(text, fx, name):
    """地面模型定义 → 挂上特效的新文本。实体 id 由文件名定（同一输入同一输出）。 | Ground model definition → new text with the effect attached. The entity id is determined by the file name (same input, same output)."""
    d = json.loads(text); assert d.get('type') == 'UnitDefinition', name
    deps = d['dependencies'].setdefault('particles', [])
    if not any(p.get('path', '').lower() == fx for p in deps): deps.append({'path': fx})
    eid = int.from_bytes(hashlib.md5(('droplight:' + name).encode()).digest()[:4], 'little') | 1
    d['entities'].append({'type': 'Entity', 'name': 'droplight', 'id': eid, 'components': [
        {'type': 'TransformDefinitionComponent', 'name': 'component_transform_droplight', 'position': {'x': 0.0, 'y': 0.0, 'z': 0.0},
         'orientation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0}, 'scale': {'x': 1.0, 'y': 1.0, 'z': 1.0}, 'inheritOnlyPosition': False},
        {'type': 'VfxDefinitionComponent', 'name': 'component_vfx_droplight', 'filename': fx, 'hardKillOnDestroy': False}]})
    return json.dumps(d, separators=(',', ':'))

def variant_path(root, asset, tier):
    """宿主改写规则的离线一份（两边必须一致）：data/hd/items/<root>/<asset>.json → data/hd/items/<root>/zswg/<tier>__<asset 里 / 换 __>.json | Offline copy of the host's rewrite rule (the two sides must match): data/hd/items/<root>/<asset>.json → data/hd/items/<root>/zswg/<tier>__<asset with / replaced by __>.json"""
    return f'data/hd/items/{root}/zswg/{tier}__' + asset.lower().replace('/', '__') + '.json'

def build(romfs, outdir, repl):
    """返回 ({逻辑路径: 文件}（新增路径带「+」前缀）, 统计)。repl 里已有同一原模型时在那份上叠加。 | Returns ({logical path: file} (new paths carry a "+" prefix), statistics). If repl already has the same original model, layer on top of that copy."""
    manifest = set()
    for line in open(os.path.join(romfs, 'data', 'build_manifest.psv'), encoding='utf-8'):
        f = line.rstrip('\n').split('|')
        if len(f) >= 11 and not line.startswith('#'): manifest.add(f[9].split(':', 1)[-1].lower())
    assets = set()
    for table in ('items.json', 'uniques.json', 'sets.json'):
        for row in json.loads(touch_layout.read_layout(romfs, None, suffix='/hd/items/' + table)[1]):
            (_, v), = row.items()
            assets.update(x.lower() for x in ([v['asset']] if 'asset' in v else [v[k] for k in ('normal', 'uber', 'ultra')]))
    out, skipped = {}, 0
    for asset in sorted(assets):
        root = next((r for r in ROOTS if f'data/hd/items/{r}/{asset}.json' in manifest), None)
        if root is None: skipped += 1; continue
        if root == 'misc' and not asset.startswith(MISC_LIT): continue
        src = f'data/hd/items/{root}/{asset}.json'
        text = open(repl[src], encoding='utf-8').read() if src in repl else touch_layout.read_layout(romfs, None, suffix='/' + src.split('/', 1)[1])[1]
        for tier, fx in TIERS.items():
            dst = variant_path(root, asset, tier)
            o = os.path.join(outdir, 'droplight_' + dst.replace('/', '_')); open(o, 'w').write(add_light(text, fx, dst)); out['+' + dst] = o
    return out, f'Variantes de luz de objetos: {len(out) // 2} modelos × 2 niveles; {skipped} modelos ausentes omitidos'

if __name__ == '__main__':   # 试验：drop_light.py <RomFS> <输出目录> → 只含本模块改动的 repl.tsv（casc_overlay.py 直接吃） | Trial: drop_light.py <RomFS> <output dir> → a repl.tsv containing only this module's changes (consumed directly by casc_overlay.py)
    romfs, out = sys.argv[1:3]; os.makedirs(out, exist_ok=True)
    r, stat = build(romfs, out, {})
    with open(os.path.join(out, 'repl.tsv'), 'w') as f:
        for k, v in r.items(): f.write(f'{k}\t{v}\n')
    print(stat)
