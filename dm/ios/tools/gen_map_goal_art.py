#!/usr/bin/env python3
"""小地图任务终点的素材（七个墓用任务日志的塔拉夏符号，其余终点用原版地图名牌，画面外贴边只写名字）。
全部取自原版资源，不画原版没有的形状：
  · 塔拉夏符号 hd/global/ui/PANEL/Quest_Log/TalRashaSymbol.sprite（7 帧；第 N 帧 = 关卡 66+N 墓门门楣上刻的那个，宿主逐门截图对过 66/67/68/70）
  · 名牌下的灰十字 = 自动地图单位十字 hd/global/ui/AUTOMAP/Units.sprite 第 6 帧（白）按 50% 透明画（原版地图上 NPC 名字下就是这个灰十字）
  · 名牌 = 原版地图给 NPC / 物件标名的样式：深色半透明底 + 名字；人名浅金 (212,196,145)、物件白字（取自原版小地图截图）；字体 hd/ui/fonts/BlizzardGlobal-V5_81.ttf
  · 名字 = 原版字符串（data/local/lng/strings/*.json 的 zhCN；英文 label_<序号>_en.png 取 enUS，西语 label_<序号>_es.png / _mx.png 取 esES / esMX，俄语 label_<序号>_ru.png 取 ruRU，韩语 label_<序号>_ko.png 取 koKR）；旧译名（自定义页「物品注释与旧译名」开着时游戏显示的名字）取 annot/oldnames.tsv
产物（构建时生成，不进仓库）：Gen/assets/NativeHUD/mapgoal/{sym_0..6.png, cross.png, heading.png（镜头实验的朝向指针，原版表情轮盘指针）, label_<序号>.png, 旧译名不同的 label_<序号>_old.png, mapgoal.json}。
用法：python3 dm/ios/tools/gen_map_goal_art.py <原版 RomFS（Update）>   （读包与解贴图走 romfs_art.py）"""
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
    # 镜头实验的人物朝向指针（front_ios.m mapHeadingSync）：原版手柄表情轮盘的指针（鎏金三角嵌宝石，本来就是从中心指向一个方向）。
    # 12 帧 = 左 / 下 / 右 / 上 四个方向 × 三种状态，取第 3 帧（常态、朝上），前端按朝向旋转
    # Facing pointer for the camera experiment (front_ios.m mapHeadingSync): the pointer of the original controller emote wheel (a gilded, jewelled
    # triangle that already points outward from a centre). 12 frames = left / down / right / up x three states; frame 3 (normal, pointing up) is taken
    # and the front end rotates it to the heading
    ptr, pfw, pn, ppitch = sprite_png('hd/global/ui/Controller/Panel/EmoteWheel/Emote_Arrow.sprite'); assert pn == 12
    # 原图是等边三角形，缩小到地图上看不出哪个是尖（jhz 真机反馈）：横向收到 0.5，成细长的箭头形，尖端一眼可辨，鎏金与宝石照旧
    # The original is an equilateral triangle, and shrunk onto the map it is unclear which corner is the tip (jhz, on the phone): narrowed to 0.5 across
    # into a slim arrowhead whose tip reads at a glance; the gilding and the jewel stay
    up = ptr.crop((3 * ppitch, 0, 3 * ppitch + pfw, ptr.height)).resize((max(1, pfw // 2), ptr.height), Image.LANCZOS); up.save(os.path.join(OUT, 'heading.png'))
    # 画面外怪物的指针（front_ios.m threatsSync，宿主 dm_cam_threats）：同一个指针按档位套原版名字配色——普通怪银白（去色）、冠军蓝（原版蓝名精英）、
    # 独特暗金（原版暗金名字的金，即指针本色）、幕首领红（原版红字）。去色后按亮度乘目标色，鎏金纹理与宝石的明暗都留着
    # Pointers for monsters off the picture (front_ios.m threatsSync, host dm_cam_threats): the same pointer in the original name colours by tier -- normal
    # silver-white (desaturated), elite blue (the original elite monster name blue), Boss gold (the original unique name gold, the pointer's own colour).
    # Desaturated and multiplied by the target colour by brightness, so the gilding texture and the jewel keep their light and shade
    def tint(im, rgb):
        a = im.getchannel('A'); g = im.convert('L').point(lambda v: min(255, v * 2))
        out = Image.merge('RGB', [g.point(lambda v, c=c: v * c // 255) for c in rgb]).convert('RGBA'); out.putalpha(a); return out
    tint(up, (225, 225, 225)).save(os.path.join(OUT, 'threat_0.png'))
    tint(up, (110, 110, 255)).save(os.path.join(OUT, 'threat_1.png'))   # 冠军（蓝名精英） | champion (blue-named elite)
    up.save(os.path.join(OUT, 'threat_2.png'))   # 独特 / 超级独特（暗金名） | unique / super unique (gold name)
    tint(up, (255, 80, 64)).save(os.path.join(OUT, 'threat_3.png'))   # 幕首领：原版红字 | act boss: the original red text
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
        ko = cache[f][key]['koKR']; label(ko, col, font).save(os.path.join(OUT, 'label_%d_ko.png' % i))   # 韩语：原版 koKR 名字（BlizzardGlobal 带韩文字形）
        man.append({'id': i, 'file': f, 'key': key, 'zhCN': zh, 'old': zo, 'enUS': en, 'esES': es, 'esMX': mx, 'ruRU': ru, 'koKR': ko, 'color': list(col)})
    json.dump({'text_px': TEXT_PX, 'symbol_frames': 7, 'labels': man}, open(os.path.join(OUT, 'mapgoal.json'), 'w'), ensure_ascii=False, indent=1)
    print('mapgoal：符号 7 帧、十字 1、朝向指针 1、名牌 %d（旧译名不同的 %d）→ %s' % (len(man), sum(m['old'] != m['zhCN'] for m in man), OUT))

if __name__ == '__main__': ROMFS = sys.argv[1]; main()
