#!/usr/bin/env python3
# 生成 dm/host/dm_bright_levels.inc：每个关卡号 → 原版画面随不随昼夜（3D 画面加亮的「室内」判据）。全部从原版数据推，不手列地图：
#   关卡的 biome：预设类（DrlgType 2）取 LvlPrest 里本关卡的预设文件 → hd/env/preset/<路径>.json 的 biomeFilename，多数票；
#                野外类（DrlgType 3）取 LvlTypes 本关卡类型的图块目录 → 该目录下预设 json 的 biomeFilename，多数票；迷宫类（DrlgType 1）不用（规则里一律 k=4）。
#   随不随昼夜：biome 的 _day 与 _night 画面配置（hd/env/vis）去掉 name / id 后内容是否相同。
# 用法：gen_bright_levels.py <原版 RomFS（Update）> > dm/host/dm_bright_levels.inc
# 构建时由 dm/host/Makefile 调，直接读玩家自己的游戏文件；生成的表不进仓库（同 dm_vault_tab.h、dm_hud_exp.h）。
# Generates dm/host/dm_bright_levels.inc: each level id → whether the original game's picture follows day and night (the "indoor" criterion for 3D picture brightening). Everything is derived from the original game's data, no maps are listed by hand:
#   A level's biome: for preset levels (DrlgType 2) take this level's preset files in LvlPrest → biomeFilename of hd/env/preset/<path>.json, majority vote;
#                for wilderness levels (DrlgType 3) take the tile directory of this level's type in LvlTypes → biomeFilename of the preset json files under that directory, majority vote; maze levels (DrlgType 1) are not used (the rule gives them k=4 across the board).
#   Whether it follows day and night: whether the biome's _day and _night picture configs (hd/env/vis) have the same content after removing name / id.
# Usage: gen_bright_levels.py <original game RomFS (Update)> > dm/host/dm_bright_levels.inc
# Called by dm/host/Makefile at build time, reading the player's own game files directly; the generated table is not committed to the repo (same as dm_vault_tab.h, dm_hud_exp.h).
import csv, io, json, os, re, sys, collections
from functools import lru_cache
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ios', 'tools'))
import touch_layout
if len(sys.argv) != 2: sys.exit('用法：gen_bright_levels.py <原版 RomFS（Update）>')
romfs = sys.argv[1]
# 游戏文件清单读一遍建索引（要读上千个小 json，逐个线性查清单太慢）：小写路径 → 清单行
# Read the game file list once and build an index (over a thousand small json files have to be read, and a linear search of the list for each one is too slow): lowercase path → list line
IDX = {}
for line in open(os.path.join(romfs, 'data', 'build_manifest.psv'), encoding='utf-8'):
    f = line.rstrip('\n').split('|')
    if len(f) >= 11: IDX.setdefault(f[9].lower().split(':', 1)[-1], f)
def rd(path):
    f = IDX.get(path.lower())
    if f is None: raise SystemExit('gen_bright_levels：游戏文件里没有 %s（RomFS 版本不对？要 3.3.94012）' % path)
    return touch_layout._read_entry(romfs, f)
ENV = 'data/hd/env/'
def ls(prefix):   # 清单里直接在这个目录下的文件名（保留原大小写） | file names in the list that sit directly under this directory (original case kept)
    out = []
    for k, f in IDX.items():
        if k.startswith(prefix) and '/' not in k[len(prefix):]: out.append(f[9].split(':', 1)[-1][len(prefix):])
    return out
def tsv(name):
    r = list(csv.reader(io.StringIO(rd('data/global/excel/' + name).decode('utf-8', 'replace')), delimiter='\t')); return r[0], r[1:]
@lru_cache(maxsize=None)
def biome_of_json(path):
    try: return os.path.basename(json.loads(rd(path)).get('biomeFilename', '')).replace('.json', '')
    except (SystemExit, Exception): return ''
def norm(path):
    def strip(o):
        if isinstance(o, dict): return {k: strip(v) for k, v in o.items() if k not in ('name', 'id')}
        if isinstance(o, list): return [strip(v) for v in o]
        return o
    return json.dumps(strip(json.loads(rd(path))), sort_keys=True)
varies = {}
for f in ls(ENV + 'biome/'):
    s = rd(ENV + 'biome/' + f).decode('utf-8', 'replace'); vis = re.findall(r'"visualDataFilename"\s*:\s*"data/hd/env/(vis/[^"]+)"', s)
    d = [v for v in vis if v.endswith('_day.json')]; n = [v for v in vis if v.endswith('_night.json')]
    if d and n and (ENV + d[0]).lower() in IDX and (ENV + n[0]).lower() in IDX: varies[f[:-5]] = norm(ENV + d[0]) != norm(ENV + n[0])
def preset_json(ds1):   # Act1/Town/TownN1.ds1 → hd/env/preset/act1/town/townN1.json（目录与文件名都不分大小写） | Act1/Town/TownN1.ds1 → hd/env/preset/act1/town/townN1.json (directory and file names are both case-insensitive)
    parts = ds1.replace('\\', '/').split('/'); p = ENV + 'preset/' + '/'.join(x.lower() for x in parts[:-1]) + '/' + parts[-1].rsplit('.', 1)[0].lower() + '.json'
    return p if p.lower() in IDX else None
@lru_cache(maxsize=None)
def dir_biome(rel):     # 图块目录（如 Act1/Outdoors）→ 该目录下预设 json 的多数 biome | Tile directory (e.g. Act1/Outdoors) → the majority biome of the preset json files under that directory
    d = ENV + 'preset/' + '/'.join(x.lower() for x in rel.replace('\\', '/').split('/')) + '/'
    c = collections.Counter(biome_of_json(d + f) for f in ls(d) if f.lower().endswith('.json'))
    return c.most_common(1)[0][0] if c else ''
lh, lv = tsv('Levels.txt'); ph, pv = tsv('LvlPrest.txt'); th, tv = tsv('LvlTypes.txt')
iid, iname, idt, ilt, iact = lh.index('Id'), lh.index('Name'), lh.index('DrlgType'), lh.index('LevelType'), lh.index('Act')
pl, pf = ph.index('LevelId'), [ph.index(f'File{k}') for k in range(1, 7)]
types = {r[th.index('Id')]: r for r in tv if len(r) > 1}
out = []
for r in lv:
    if len(r) <= ilt or not r[iid].strip().isdigit(): continue
    lid, dt = int(r[iid]), r[idt].strip()
    if not dt.isdigit(): dt = '0'
    c = collections.Counter()
    if dt == '2':
        for p in pv:
            if len(p) > pf[-1] and p[pl].strip() == str(lid):
                for k in pf:
                    j = preset_json(p[k]) if p[k] not in ('', '0') else None
                    if j: c[biome_of_json(j)] += 1
    elif dt == '3':
        t = types.get(r[ilt].strip())
        if t:
            for k in range(2, len(t)):
                m = re.match(r'(Act\d|Expansion)[/\\]([A-Za-z]+)[/\\]', t[k] or '')
                if m: c[dir_biome(m.group(1) + '/' + m.group(2))] += 1
    c.pop('', None)
    b = c.most_common(1)[0][0] if c else ''
    out.append((lid, r[iname], dt, int(r[iact] or 0), b, int(varies[b]) if b in varies else -1))
print('// 由 dm/tools/gen_bright_levels.py 在构建时从玩家自己的游戏文件（Levels / LvlPrest / LvlTypes 与 hd/env 画面配置）生成，勿手改，不进仓库。')
print('// 每行：关卡号、幕号（Levels.txt Act，0 起）、DrlgType（1 迷宫 2 预设 3 野外）、原版画面随不随昼夜（1 随 / 0 不随 / -1 未找到 biome）——biome、关卡名')
for lid, name, dt, act, b, v in out: print(f'{{ {lid}, {act}, {dt or 0}, {v} }},   // {b or "（无）"}，{name}')
