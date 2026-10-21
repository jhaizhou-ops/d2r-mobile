#!/usr/bin/env python3
"""小地图任务终点的素材（七个墓用任务日志的塔拉夏符号，其余终点用原版地图名牌，画面外贴边只写名字）。
全部取自原版资源，不画原版没有的形状：
  · 塔拉夏符号 hd/global/ui/PANEL/Quest_Log/TalRashaSymbol.sprite（7 帧；第 N 帧 = 关卡 66+N 墓门门楣上刻的那个，宿主逐门截图对过 66/67/68/70）
  · 名牌下的灰十字 = 自动地图单位十字 hd/global/ui/AUTOMAP/Units.sprite 第 6 帧（白）按 50% 透明画（原版地图上 NPC 名字下就是这个灰十字）
  · 名牌 = 原版地图给 NPC / 物件标名的样式：深色半透明底 + 名字；人名浅金 (212,196,145)、物件白字（取自原版小地图截图）；字体 hd/ui/fonts/BlizzardGlobal-V5_81.ttf
  · 名字 = 原版字符串（data/local/lng/strings/*.json 的 zhCN；英文 label_<序号>_en.png 取 enUS，西语 label_<序号>_es.png / _mx.png 取 esES / esMX，俄语 label_<序号>_ru.png 取 ruRU）；旧译名（自定义页「物品注释与旧译名」开着时游戏显示的名字）取 annot/oldnames.tsv
产物（构建时生成，不进仓库）：Gen/assets/NativeHUD/mapgoal/{sym_0..6.png, cross.png, label_<序号>.png, 旧译名不同的 label_<序号>_old.png, mapgoal.json}。
用法：python3 dm/ios/tools/gen_map_goal_art.py <原版 RomFS（Update）>   （读包与解贴图走 romfs_art.py）
'''Art for minimap quest destinations (the seven tombs use Tal Rasha symbols from the quest log, other destinations use original map name plates, off-screen edge markers show only the name).
All taken from original assets, no shapes the original doesn't have:
  - Tal Rasha symbol hd/global/ui/PANEL/Quest_Log/TalRashaSymbol.sprite (7 frames; frame N = the one carved on the tomb lintel of level 66+N, host checked door by door with screenshots for 66/67/68/70)
  - Gray cross under the name plate = automap unit cross hd/global/ui/AUTOMAP/Units.sprite frame 6 (white) drawn at 50% opacity (on the original map this gray cross is under NPC names)
  - Name plate = the style the original map uses to label NPCs / objects: dark translucent background + name; person names light gold (212,196,145), objects white (from original minimap screenshots); font hd/ui/fonts/BlizzardGlobal-V5_81.ttf
  - Names = original strings (zhCN of data/local/lng/strings/*.json; English label_<n>_en.png from enUS, Spanish label_<n>_es.png / _mx.png from esES / esMX, Russian label_<n>_ru.png from ruRU); old names (names the game shows when "Item annotations and old names" is on in the custom page) from annot/oldnames.tsv
Output (generated at build time, not in the repo): Gen/assets/NativeHUD/mapgoal/{sym_0..6.png, cross.png, label_<n>.png, label_<n>_old.png for differing old names, mapgoal.json}.
Usage: python3 dm/ios/tools/gen_map_goal_art.py <original RomFS (Update)>   (package reading and texture decoding via romfs_art.py)'''"""
import csv, json, os, struct, sys
from PIL import Image, ImageDraw, ImageFont
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import romfs_art
from gen_paths import gen_dir
OUT = gen_dir('NativeHUD', 'mapgoal'); ROMFS = None
NPC, OBJ = (212, 196, 145), (255, 255, 255)
# 名牌表：序号是宿主 dm_world.c 终点表里的「名牌号」，只许往后加、不许改序号
# Label table: the index is the "label number" in the goal table of the host's dm_world.c; entries may only be appended, indices must not be changed
LABELS = [
    ('levels', "Tal Rasha's Tomb", OBJ),   # 0  画面外的真墓（画面内用符号） | 0  The true tomb when off screen (on screen the symbol is used)
    ('monsters', 'Bloodraven', NPC),       # 1  血鸦 | 1  Blood Raven
    ('objects', 'StoneAlpha', OBJ),        # 2  凯恩之石（石柱） | 2  Cairn Stones (stone pillars)
    ('objects', 'Inifuss', OBJ),           # 3  因纽斯之树 | 3  Tree of Inifuss
    ('monsters', 'The Countess', NPC),     # 4  女伯爵 | 4  The Countess
    ('item-names', 'hdm', OBJ),            # 5  赫拉迪姆之锤 | 5  Horadric Malus
    ('monsters', 'Radament', NPC),         # 6
    ('item-names', 'box', OBJ),            # 7  赫拉迪姆魔盒 | 7  Horadric Cube
    ('item-names', 'Staff of Kings', OBJ), # 8  列王之杖 | 8  Staff of Kings
    ('objects', 'taintedsunaltar', OBJ),   # 9  被污染的太阳祭坛 | 9  Tainted Sun Altar
    ('monsters', 'The Summoner', NPC),     # 10 召唤者 | 10 The Summoner
    ('item-names', 'qey', OBJ),            # 11 卡林姆的晶体 | 11 Khalim's Eye
    ('item-names', 'qbr', OBJ),            # 12 卡林姆的智慧 | 12 Khalim's Brain
    ('item-names', 'qhr', OBJ),            # 13 卡林姆的生命核心 | 13 Khalim's Heart
    ('objects', 'compellingorb', OBJ),     # 14 强迫之球 | 14 Compelling Orb
    ('item-names', 'bbb', OBJ),            # 15 兰姆·艾森之书 | 15 Lam Esen's Tome
    ('levels', 'Ruined Temple', OBJ),      # 16 市集里通往毁灭的神庙的入口 | 16 Entrance in the Bazaar leading to the Ruined Temple
    ('monsters', 'Izual', NPC),            # 17
    ('objects', 'Hellforge', OBJ),         # 18 地狱熔炉 | 18 Hellforge
    ('item-names', 'Seal', OBJ),           # 19 封印 | 19 Seal
    ('monsters', 'Siege Boss', NPC),       # 20 督军山克 | 20 Shenk the Overseer
    ('objects', 'cagedwussie1', OBJ),      # 21 笼子（野蛮人囚笼） | 21 Cage (barbarian prison cage)
    ('npcs', 'Drehya', NPC),               # 22 安雅 | 22 Anya
    ('objects', 'orifice', OBJ),           # 23 真墓里的法杖底座 | 23 Staff orifice in the true tomb
    ('objects', 'gidbinn altar', OBJ),     # 24 吉德宾祭坛 | 24 Gidbinn altar
    ('monsters', 'Andariel', NPC),         # 25 安达莉尔 | 25 Andariel
    ('monsters', 'Duriel', NPC),           # 26 督瑞尔 | 26 Duriel
    ('monsters', 'Mephisto', NPC),         # 27 墨菲斯托 | 27 Mephisto
    ('monsters', 'Baal', NPC),             # 28 巴尔（王座上） | 28 Baal (on the throne)
    ('objects', 'ancientsaltar', OBJ),     # 29 天坛（召出远古三人的祭坛） | 29 Altar of the Heavens (the altar that summons the three Ancients)
    ('npcs', 'Nihlathak', NPC),            # 30 尼拉塞克（按他的出生点物件定位） | 30 Nihlathak (located by his spawn point object)
]
TEXT_PX, PAD_X, PAD_Y, BOX_A = 48, 10, 6, 150   # 按 48 像素字出图，前端按屏高缩放（原版地图名牌字高约屏高的 1/53） | Rendered with 48-pixel text, the front end scales by screen height (the original game's map label glyph height is about 1/53 of screen height)

def sprite_png(path):
    b = romfs_art.read(ROMFS, path); assert b[:4] == b'SpA1'
    code, fw, w, h, frames = struct.unpack_from('<HHII4xH', b, 4); assert code == 0x3d, (path, code)
    return romfs_art.dxt5(b, w, h), fw, frames, w // frames

def strings(name):
    return {r['Key']: r for r in json.loads(romfs_art.read(ROMFS, 'local/lng/strings/%s.json' % name).decode('utf-8-sig'))}

def label(text, col, font):
    d0 = ImageDraw.Draw(Image.new('RGBA', (1, 1))); w = int(d0.textlength(text, font=font)) + 2 * PAD_X; h = TEXT_PX + 2 * PAD_Y
    im = Image.new('RGBA', (w, h), (0, 0, 0, BOX_A)); ImageDraw.Draw(im).text((w / 2, h / 2), text, font=font, fill=col + (255,), anchor='mm')
    return im

def main():
    os.makedirs(OUT, exist_ok=True)
    sym, fw, n, pitch = sprite_png('hd/global/ui/PANEL/Quest_Log/TalRashaSymbol.sprite'); assert n == 7
    for i in range(7): sym.crop((i * pitch, 0, i * pitch + fw, sym.height)).save(os.path.join(OUT, 'sym_%d.png' % i))
    units, ufw, un, upitch = sprite_png('hd/global/ui/AUTOMAP/Units.sprite'); assert un == 8
    cross = units.crop((6 * upitch, 0, 6 * upitch + ufw, units.height)); a = cross.split()[3].point(lambda v: v // 2); cross.putalpha(a); cross.save(os.path.join(OUT, 'cross.png'))
    font = ImageFont.truetype(romfs_art.font(ROMFS, 'BlizzardGlobal-V5_81.ttf'), TEXT_PX)
    oldp = os.path.join(HERE, 'annot', 'oldnames.tsv')   # 旧译名对照表可以没有（公开仓库不带）：没有就都用现行名 | the old-name table may be absent (the public repository does not carry it): without it the current names are used throughout
    old = {(r[0], r[1]): r[3] for r in csv.reader(open(oldp, encoding='utf-8'), delimiter='\t') if len(r) >= 4} if os.path.isfile(oldp) else {}
    cache, man = {}, []
    for i, (f, key, col) in enumerate(LABELS):
        if f not in cache: cache[f] = strings(f)
        zh = cache[f][key]['zhCN']; zo = old.get((f, key), zh)
        label(zh, col, font).save(os.path.join(OUT, 'label_%d.png' % i))
        if zo != zh: label(zo, col, font).save(os.path.join(OUT, 'label_%d_old.png' % i))   # 旧译名不同才出（前端没有 _old 就用 label_<序号>） | Only emitted when the old translated name differs (without _old the front end uses label_<index>)
        en = cache[f][key]['enUS']; label(en, col, font).save(os.path.join(OUT, 'label_%d_en.png' % i))   # 英文版：原版 enUS 名字，游戏语言不是中文 / 西语时前端用它 | English version: original enUS names, used by the front end when the game language is neither Chinese nor Spanish
        es = cache[f][key]['esES']; label(es, col, font).save(os.path.join(OUT, 'label_%d_es.png' % i))   # 西语：原版 esES / esMX 名字（两套用词不同，各出一张） | Spanish: original esES / esMX names (the two use different wording, one image each)
        mx = cache[f][key]['esMX']; label(mx, col, font).save(os.path.join(OUT, 'label_%d_mx.png' % i))
        ru = cache[f][key]['ruRU']; label(ru, col, font).save(os.path.join(OUT, 'label_%d_ru.png' % i))   # 俄语：原版 ruRU 名字 | Russian: original ruRU names
        man.append({'id': i, 'file': f, 'key': key, 'zhCN': zh, 'old': zo, 'enUS': en, 'esES': es, 'esMX': mx, 'ruRU': ru, 'color': list(col)})
    json.dump({'text_px': TEXT_PX, 'symbol_frames': 7, 'labels': man}, open(os.path.join(OUT, 'mapgoal.json'), 'w'), ensure_ascii=False, indent=1)
    print('mapgoal：符号 7 帧、十字 1、名牌 %d（旧译名不同的 %d）→ %s' % (len(man), sum(m['old'] != m['zhCN'] for m in man), OUT))

if __name__ == '__main__': ROMFS = sys.argv[1]; main()
