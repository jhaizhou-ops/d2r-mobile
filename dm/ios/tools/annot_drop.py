"""地上掉落名（筛选逻辑和规则复刻红叶；不抄红叶的字，信息一条不少，全从游戏表算，用我们自己的措辞）。
红叶的「筛选」就是改名字（它什么都不藏）：低价值消耗品缩短、值钱 / 要用的加色、加来源与用途、底材加档次与价值。逐类对应如下：
  药水：治疗 / 法力 1–5 级缩成 微 / 小 / 中 / 大 / 超 + 红 / 蓝（超级加色）；复苏 小紫 / 大紫；解毒药水去「药水」。
  卷轴：回城、鉴定两种常掉的缩短；箭矢 / 弩矢缩成「箭」「弩矢」。
  宝石 / 骷髅：缩成「档-颜色」；碎裂 / 有瑕疵 / 标准三档加「合 N」= 这颗在魔盒里升级符文时合出第几号（cubemain.txt 的符文升级配方）。
  符文：现行名「中文 英文 #号」上色，24 号起加框线、27 号起多空行、28 号起框线加长（越值钱越显眼）。
  钥匙 / 首领器官 / 精华：来源首领由掉落表反查（treasureclassex.txt 逐层往上找，到 monstats / superuniques 的地狱掉落列），名字取字符串表；
    钥匙另标首领所在幕，任务物品（ua1–ua5）按 misc.txt 英文名里的 Act 号标幕。
  底材（武器 / 护甲）：档次与轻中重由前面的「底材档次」步骤写好；这里加 ★（值得留的底材，名单与理由见 VALUE_BASES）、价值数字、顶级符文之语、
    职业标枪的魔法词缀上限、合成手套的标志属性（cubemain.txt 的手套合成配方）。
名字一律取字符串表的现行值（已做过旧译名 / 符文序号）。
措辞按语言列（WORDS：zhCN / esES / esMX），s.col 是哪列就用哪套、名字也取那一列；规则与中文相同。西语措辞只用原版拉丁字体（Formal436BT / Exocet）里有的字：
没有 ★ 〈〉 →，所以「值得留」用 *、合成标志用 « »。游戏把多行名字倒着画（最后一行在最上面），所以「说明\\n名字」画出来是名字在上、说明在下。
Ground drop names (filter logic and rules replicate Hongye (红叶); no text copied from Hongye, no information dropped, everything computed from game tables, in our own wording).
Hongye's "filter" is just renaming (it hides nothing): low-value consumables shortened, valuable / useful items colored, source and use added, bases get grade and value. Per category:
  Potions: healing / mana levels 1–5 shortened to tiny / small / medium / large / super + red / blue (super is colored); rejuvenation small purple / large purple; antidote potion drops "potion".
  Scrolls: the two common drops, town portal and identify, are shortened; arrows / bolts shortened to "arrows" / "bolts".
  Gems / skulls: shortened to "grade-color"; the chipped / flawed / standard grades get "cube N" = which rune number this gem produces when upgrading runes in the Horadric Cube (rune upgrade recipes in cubemain.txt).
  Runes: current name "Chinese English #number" colored; from #24 a border line is added, from #27 extra blank lines, from #28 a longer border (the more valuable, the more visible).
  Keys / boss organs / essences: source boss found by reverse lookup in drop tables (treasureclassex.txt walked upward level by level to the Hell drop columns of monstats / superuniques), names taken from the string table;
    keys also note the boss's act; quest items (ua1–ua5) note the act from the Act number in the English name in misc.txt.
  Bases (weapons / armor): grade and light / medium / heavy written by the earlier "base grade" step; here add ★ (bases worth keeping, list and reasons in VALUE_BASES), value number, top runewords,
    magic affix caps of class javelins, signature stats of crafted gloves (glove crafting recipes in cubemain.txt).
Names always take the current value from the string table (old names / rune numbers already applied).
Wording per language column (WORDS: zhCN / esES / esMX): whichever column s.col is, use that set, names also from that column; rules same as Chinese. Spanish wording only uses characters present in the original Latin fonts (Formal436BT / Exocet):
no ★ 〈〉 →, so "worth keeping" uses * and craft signature uses « ». The game draws multi-line names bottom-up (last line on top), so a "note, line break, name" string renders with the name on top and the note below."""
import math, re

FILES = ('item-names.json', 'item-runes.json', 'chinese-overlay.json')   # 加载顺序：后加载的同键覆盖前面的 | Load order: a later-loaded file overrides earlier ones for the same key
NAME_FILES = FILES + ('monsters.json', 'npcs.json')
RED, BLUE, BLUE2, PURPLE, ORANGE, GREEN = 'ÿc1', 'ÿc3', 'ÿc5', 'ÿc;', 'ÿc8', 'ÿc2'
SIZE = ('微', '小', '中', '大', '超')   # 药水 1–5 级 | Potion levels 1–5
SHORT = {'tsc': '回城', 'aqv': '箭', 'cqv': '弩矢', 'rvs': PURPLE + '小紫', 'rvl': PURPLE + '大紫'}
STRIP = {'isc': '卷轴', 'yps': '药水'}   # 常掉的低价值件：去掉后缀 | Commonly dropped low-value items: strip the suffix
GEM_COLOR = {'gema': '紫', 'gemt': '黄', 'gems': '蓝', 'geme': '绿', 'gemr': '红', 'gemd': '钻', 'gemz': '骷髅'}
GEM_GRADE = (('碎裂', ''), ('裂开', ''), ('', ''), ('无瑕', RED), ('完美', RED))   # 档：碎裂 / 有瑕疵 / 标准 / 无瑕 / 完美 | Grades: chipped / flawed / standard / flawless / perfect
GEM_GRADE_CH = {'c': 0, 'f': 1, 's': 2, 'l': 3, 'z': 3, 'p': 4}   # 宝石码第 2 字（gzv：紫宝石的无瑕用 z） | 2nd character of the gem code (gzv: amethyst uses z for flawless)
SKULL_GRADE_CH = {'c': 0, 'f': 1, 'u': 2, 'l': 3, 'z': 4}   # 骷髅码第 3 字（skl 无瑕、skz 完美） | 3rd character of the skull code (skl flawless, skz perfect)
HIGH_RUNE = 24   # 从这一号起加上下框线（值钱的高级符文）；27 号起上下多空两行、28 号起框线加长 | From this number on, add frame lines above and below (valuable high runes); from 27 on, two extra blank lines above and below; from 28 on, longer frame lines
SOURCE_NOTE = {'pk1': '火炬钥匙', 'pk2': '火炬钥匙', 'pk3': '火炬钥匙', 'dhn': '地狱火炬材料', 'bey': '地狱火炬材料', 'mbr': '地狱火炬材料',
               'tes': '合成赦免勋章', 'ceh': '合成赦免勋章', 'bet': '合成赦免勋章', 'fed': '合成赦免勋章'}
SOURCE_COLOR = {'pk1': RED, 'pk2': ORANGE, 'pk3': GREEN}
BOSS_ACT = {'The Countess': 1, 'The Summoner': 2, 'Nihlathak Boss': 5}   # 首领所在幕（地图事实） | Act the boss is in (map fact)
# 值得留的底材（我们自己的判据，按用途分组）：
#   法珠（法师专用、可 4 孔、自带法术词缀）；刺客高阶爪（3 孔做混沌 / 狂怒、或高技能魔法爪）；头环（可出 +2 技能 / 3 孔）；
#   精英头盔里出顶级暗金 / 符文之语的三顶；4 孔精英身甲与力量需求最低的扩展身甲（做谜团 / 刚毅）；6 孔女族长之矛；4 孔圣骑士皇家盾牌。
# 「值」= 这件做卓越品质（+1 最大值后 ×1.15）时的上限；身甲做谜团不做无形，头盔 / 武器给佣兵用按无形（再 ×1.5）算。武器给伤害、护甲给防御。
# Bases worth keeping (our own criteria, grouped by use):
#   orbs (sorceress-only, can have 4 sockets, come with spell automods); high-tier assassin claws (3 sockets for Chaos / Fury, or high-skill magic claws); circlets (can roll +2 skills / 3 sockets);
#   the three elite helms that yield top uniques / runewords; 4-socket elite body armor and the exceptional body armor with the lowest strength requirement (for Enigma / Fortitude); 6-socket Matriarchal Spear; 4-socket paladin Royal Shield.
# "Value" = the cap when this piece is superior quality (+1 max, then ×1.15); body armor is for Enigma and not ethereal, helms / weapons are for the mercenary and computed as ethereal (a further ×1.5). Weapons give damage, armor gives defense.
VALUE_BASES = {
    **{c: {} for c in ('ob1', 'ob2', 'ob3', 'ob4', 'ob5', 'ob6', 'ob7', 'ob8', 'ob9', 'oba', 'obb', 'obc', 'obd', 'obe', 'obf')},
    **{c: {} for c in ('9lw', '9tw', '7lw', '7tw', 'ci1', 'ci2', 'ci3', 'uhm', 'usk', 'utu', 'upl', 'uth', 'uul', 'uar', 'utp', 'amd', 'paa')},
    'xtp': {'value': 'range'},
    'urn': {'value': 'max', 'eth': True},
}
# 顶级符文之语：每件底材列出我们认为值得为它留底的几个（runes.txt 核过底材类型，能做才写）
# Top runewords: for each base, list the few we consider worth keeping the base for (base types checked against runes.txt; only written if it can be made)
TOP_RUNEWORDS = {'7wa': ('Oath', 'Death', 'Breath of the Dying')}
WEAPON_VALUE = {'7wa': {'value': 'range', 'eth': True}}
FRAMED = ('ci3',)   # 头环里最高阶的权冠：可出 +2 技能 + 2 孔以上的顶级头环底 | The highest-tier circlet, the Diadem: a top circlet base that can roll +2 skills + 2 or more sockets
CLASS_MAGIC = ('ama', 'amf')   # 职业标枪里扩展 / 精英最高阶的两把：标出魔法件能出的最高技能与攻速 | The two highest-tier exceptional / elite class javelins: mark the highest skill and attack speed a magic one can roll
CRAFT_GLOVE_MODS = {'crush': '压碎', 'mana-kill': '杀敌回蓝'}   # 手套合成配方的标志属性里值得标的两种（鲜血 / 施法者合成） | The two signature properties of glove crafting recipes that are worth marking (Blood / Caster crafts)
_ROMAN = ('', 'I', 'II', 'III', 'IV', 'V')
# 按语言列的措辞。pot：药水「尺寸 + 颜色」的拼法；strip：去掉名字里的这一段（正则）后首字母大写；gem：宝石「档 / 颜色」的拼法；
# source：来源说明（西语里的 {torch} / {token} 填原版该列的「地狱火炬」「赦免之证」名字）；act：「第 N 幕」（esES 原版写罗马数字、esMX 写阿拉伯数字）
# Wording per language column. pot: how potion "size + color" is spelled; strip: remove this part of the name (regex) and capitalize the first letter; gem: how gem "grade / color" is spelled;
# source: source note ({torch} / {token} in Spanish are filled with that column's original names for "Hellfire Torch" and "Token of Absolution"); act: "Act N" (original esES uses Roman numerals, esMX Arabic numerals)
_ES = {
    'pot': '{c} {s}', 'pot_color': {'hpot': 'Roja', 'mpot': 'Azul'},
    'short': {'tsc': 'Portal', 'rvs': PURPLE + 'Morada', 'rvl': PURPLE + 'Morada total'},
    'strip': {'isc': r'^Pergamino de ', 'yps': r'^Poción '},
    'gem': '{c} {w}', 'gem_color': {'gema': 'Amat.', 'gemt': 'Topa.', 'gems': 'Zaf.', 'geme': 'Esm.', 'gemr': 'Rubí', 'gemd': 'Diam.', 'gemz': 'Crán.'},
    'gem_rune': ' (Runa #%d)',
    'source': dict(**{c: 'Llave para {torch}' for c in ('pk1', 'pk2', 'pk3')}, **{c: 'Material para {torch}' for c in ('dhn', 'bey', 'mbr')},
                   **{c: 'Para crear {token}' for c in ('tes', 'ceh', 'bet', 'fed')}),
    'or': ' o ',
    'class_magic': 'Mágico: hab. +%d, vel. ataque +%d', 'weapon_value': 'Vel. ataque %s · Daño %d-%d', 'def_range': 'Defensa %d-%d', 'def': 'Defensa %d',
    'can_make': 'Para ', 'star': '*', 'craft': ' «Artesanal: %s»', 'craft_mods': {'crush': 'golpe aplastante', 'mana-kill': 'maná al matar'},
}
WORDS = {
    'zhCN': {'pot': '{s}{c}', 'size': SIZE, 'pot_color': {'hpot': '红', 'mpot': '蓝'}, 'short': SHORT, 'strip': {k: re.escape(v) + '$' for k, v in STRIP.items()},
             'gem': '{w}-{c}', 'gem_color': GEM_COLOR, 'grade': tuple(w for w, _ in GEM_GRADE), 'gem_rune': ' (合%d)',
             'source': SOURCE_NOTE, 'or': '或', 'act': lambda n: ' 第%s幕' % n,
             'class_magic': '魔法 技能+%d 攻速+%d', 'weapon_value': '攻速 %s · 伤害 %d-%d', 'def_range': '防御 %d-%d', 'def': '防御 %d',
             'can_make': '可做 ', 'star': '★', 'craft': ' 〈合成·%s〉', 'craft_mods': CRAFT_GLOVE_MODS},
    'esES': dict(_ES, size=('menor', 'ligera', '', 'superior', 'súper'), grade=('frag.', 'estrop.', '', 'sin def.', 'perf.'),
                 act=lambda n: ' Acto %s' % _ROMAN[int(n)]),
    'esMX': dict(_ES, size=('menor', 'leve', '', 'mayor', 'especial'), grade=('mell.', 'imperf.', '', 'impec.', 'perf.'),
                 act=lambda n: ' Acto %s' % n),
}


def _strings_get(s, key):
    v = [s.get(f, key) for f in NAME_FILES if s.has(f, key)]
    return v[-1] if v else None


class Tables:
    """一次性读好要用的游戏表。 | Read all the game tables that are needed, once."""
    def __init__(self, romfs):
        from annot_strings import excel
        self.misc = {r['code']: r for r in excel(romfs, 'misc.txt') if r.get('code')}
        self.weap = {r['code']: r for r in excel(romfs, 'weapons.txt') if r.get('code')}
        self.armor = {r['code']: r for r in excel(romfs, 'armor.txt') if r.get('code')}
        self.types = {r['Code']: r for r in excel(romfs, 'itemtypes.txt') if r.get('Code')}
        self.cube = excel(romfs, 'cubemain.txt')
        self.runes = {r['*Rune Name']: r for r in excel(romfs, 'runes.txt') if r.get('*Rune Name')}
        self.tc = excel(romfs, 'treasureclassex.txt')
        self.monstats = excel(romfs, 'monstats.txt'); self.superuniques = excel(romfs, 'superuniques.txt')
        self.automagic = excel(romfs, 'automagic.txt'); self.prefix = excel(romfs, 'magicprefix.txt'); self.suffix = excel(romfs, 'magicsuffix.txt')
        self.parents = {}
        for r in self.tc:
            for k in range(1, 11):
                i = r.get('Item%d' % k, '')
                if i: self.parents.setdefault(i, set()).add(r['Treasure Class'])

    def ancestors(self, t):
        out, st = set(), [t]
        while st:
            x = st.pop()
            if x and x not in out:
                out.add(x); st += [self.types.get(x, {}).get('Equiv1', ''), self.types.get(x, {}).get('Equiv2', '')]
        return out

    def gem_rune(self, code):
        """这颗宝石在哪条符文升级配方里当辅料：返回合出的符文号。 | Which rune upgrade recipe uses this gem as the secondary ingredient: returns the number of the resulting rune."""
        for r in self.cube:
            ins = [(r.get('input %d' % k, '') or '').split(',')[0] for k in range(1, 8)]; out = r.get('output', '')
            if code in ins and re.fullmatch(r'r\d\d', out) and any(re.fullmatch(r'r\d\d', i) for i in ins): return int(out[1:])
        return None

    def sources(self, code):
        """掉这件的首领 [(表, 内部名, 名字键)]（地狱难度）：从直接列出它的掉落表逐层往上找，再找哪个超级独特怪 / 怪的地狱掉落表在里面。 | Bosses that drop this piece [(table, internal name, name key)] (Hell difficulty): starting from the treasure class that lists it directly, search upward level by level, then find which super unique / monster has its Hell treasure class in there."""
        seen, st = set(), [code]
        while st:
            for p in self.parents.get(st.pop(), ()):
                if p not in seen: seen.add(p); st.append(p)
        out = []
        for r in self.superuniques:
            if any(r.get(c) in seen for c in r if c.startswith('TC') and '(H)' in c): out.append(('su', r['Superunique'], r.get('Name')))
        if out: return out
        for r in self.monstats:
            if any(r.get(c) in seen for c in r if c.startswith('TreasureClass') and '(H)' in c): out.append(('mon', r.get('Id'), r.get('NameStr')))
        return out

    def class_magic(self, code):
        """职业物品魔法件能出的最高技能（自动词缀 + 前缀的技能页加成）与最高攻速（后缀）。 | Highest skill a magic class item can roll (skill-tab bonus from automod + prefix) and highest attack speed (suffix)."""
        a = self.ancestors(self.weap[code]['type'])
        def best(tab, mods, spawn):
            v = 0
            for r in tab:
                if spawn and r.get('spawnable') != '1': continue
                its = {r.get('itype%d' % k, '') for k in range(1, 8)}; ex = {r.get('etype%d' % k, '') for k in range(1, 6)}
                if not (its & a) or (ex & a): continue
                for k in range(1, 4):
                    if r.get('mod%dcode' % k) in mods: v = max(v, int(r.get('mod%dmax' % k) or 0))
            return v
        return best(self.automagic, ('skilltab',), False) + best(self.prefix, ('skilltab',), True), best(self.suffix, ('swing1', 'swing2', 'swing3'), True)

    def craft_glove(self, code):
        """这双手套所在的一族（普 / 扩 / 精）是哪条手套合成配方的底材：返回配方标志属性里值得标的那一个的属性码（CRAFT_GLOVE_MODS 的键）。 | Which glove crafting recipe uses this glove's family (normal / exceptional / elite) as its base: return the stat code of the one worth marking among the recipe's signature stats (a key of CRAFT_GLOVE_MODS)."""
        r = self.armor[code]; fam = {r.get('normcode'), r.get('ubercode'), r.get('ultracode')}
        for c in self.cube:
            if (c.get('input 1', '') or '').split(',')[0] in fam:
                for k in range(1, 6):
                    if c.get('mod %d' % k) in CRAFT_GLOVE_MODS: return c.get('mod %d' % k)
        return None


def _rune_line(code, name):
    m = re.fullmatch(r"(.+) ([A-Za-z']+) (#\d+)", name)
    if not m: return None
    i, txt = int(code[1:]), ORANGE + m.group(1) + ' ' + BLUE2 + m.group(2) + GREEN + m.group(3)
    if i < HIGH_RUNE: return txt
    bar, gap = ORANGE + '—' * (13 if i >= 28 else 9), '\n' * (4 if i >= 27 else 2)
    return bar + gap + txt + gap + bar


def misc_rule(T, s, code, name):
    """杂物类一个物品码的掉落名；None = 不改。 | Drop name for one miscellaneous item code; None = no change."""
    row = T.misc[code]; t = row.get('type', ''); L = WORDS[s.col]
    if code in L['short']: return L['short'][code]
    m = re.search(L['strip'][code], name) if code in L['strip'] else None
    if m: r = name[:m.start()] + name[m.end():]; return r[:1].upper() + r[1:]
    if t in ('hpot', 'mpot') and code[-1:].isdigit():
        i = int(code[-1]) - 1; w = L['pot'].format(s=L['size'][i], c=L['pot_color'][t]).strip()
        return (RED if t == 'hpot' else BLUE) + w if i == 4 else w
    if t in GEM_COLOR:
        g = SKULL_GRADE_CH.get(code[2]) if t == 'gemz' else GEM_GRADE_CH.get(code[1])
        if g is None: return None
        word, color = L['grade'][g], GEM_GRADE[g][1]
        base = color + L['gem'].format(w=word, c=L['gem_color'][t]) if word else name
        n = T.gem_rune(code) if g <= 2 else None
        return base + (L['gem_rune'] % n if n else '')
    if t == 'rune' and re.fullmatch(r'r\d\d', code): return _rune_line(code, name)
    if code in SOURCE_NOTE:
        src = T.sources(code)
        if not src: raise SystemExit('annot_drop：%s 在掉落表里找不到来源首领' % code)
        who = L['or'].join(dict.fromkeys(_strings_get(s, key) or key for _, _, key in src))
        act = next((BOSS_ACT[x[1]] for x in src if x[1] in BOSS_ACT), None)
        color = SOURCE_COLOR.get(code, GREEN)
        nm = lambda f, k: re.sub(r'\u00ffc.', '', s.get(f, k).split('\n')[-1])   # 前面的注释步骤给暗金名加了范围行：只取名字那一行 | An earlier annotation step added a range line to unique names: take only the name line
        note = L['source'][code].format(torch=nm('item-names.json', 'Hellfire Torch'), token=nm('item-modifiers.json', 'toa')) if s.col != 'zhCN' else L['source'][code]
        return color + '[' + who + ' · ' + note + ']\n' + color + name + (L['act'](act) if act else '')
    m = re.search(r'Act ?(\d)', row.get('name', ''))
    if m and t == 'ques' and code.startswith('ua'): return name + L['act'](m.group(1))
    return None


def base_rule(T, s, code, name):
    """武器 / 护甲一个物品码的掉落名（在底材档次之后）；None = 不改。 | Drop name for one weapon / armor item code (after the base tier); None = no change."""
    parts = []; L = WORDS[s.col]
    w, a = T.weap.get(code), T.armor.get(code)
    if code in CLASS_MAGIC:
        sk, ias = T.class_magic(code); parts.append(L['class_magic'] % (sk, ias))
    vb = VALUE_BASES.get(code) or WEAPON_VALUE.get(code)
    if vb and vb.get('value'):
        k = 1.5 if vb.get('eth') else 1.0
        if w:
            lo, hi = int(w['mindam'] or w['2handmindam']), int(w['maxdam'] or w['2handmaxdam'])
            parts.append(L['weapon_value'] % (w.get('speed') or '0', lo, math.floor(math.ceil(hi * k) * 1.15)))
        else:
            lo, hi = int(a['minac']), int(a['maxac']); top = math.floor(math.floor((hi + 1) * k) * 1.15)
            parts.append((L['def_range'] % (lo, top)) if vb['value'] == 'range' else L['def'] % top)
    if code in TOP_RUNEWORDS:
        names = []
        for rw in TOP_RUNEWORDS[code]:
            r = T.runes[rw]; its = {r.get('itype%d' % k, '') for k in range(1, 7)}
            if not (its & T.ancestors((w or a)['type'])): raise SystemExit('annot_drop：%s 做不了 %s' % (code, rw))
            nm = (_strings_get(s, r['Name']) or rw).split('\n')[-1]   # 前面的注释步骤给符文之语名加了范围 / 点评行：只取名字那一行 | Earlier annotation steps added range / review lines to the runeword name: take only the name line
            names.append(re.sub(r'\u00ffc.', '', nm))
        parts.append(L['can_make'] + ' / '.join(names))
    glove = T.craft_glove(code) if a and a.get('type') == 'glov' else None
    out = name
    if code in VALUE_BASES: out = L['star'] + out
    if glove: out += L['craft'] % L['craft_mods'][glove]
    if parts: out = '[' + ' · '.join(parts) + ']\n' + out
    if code in FRAMED: out = '—' * 7 + '\n' + out + '\n' + '—' * 7   # 最顶级的几件上下加框线 | Add frame lines above and below the few top-tier pieces
    return out if out != name else None


def ann_drop(s, romfs):
    """s = 已做完前面步骤的 Strings。返回改写条数。 | s = Strings with the earlier steps already applied. Returns the number of rewritten entries."""
    T = Tables(romfs); n = 0
    for tab, rule in ((T.misc, misc_rule), (T.weap, base_rule), (T.armor, base_rule)):
        for code, row in tab.items():
            key = row.get('namestr') or code
            cur = [s.get(f, key) for f in FILES if s.has(f, key)]
            if not cur: continue
            new = rule(T, s, code, cur[-1])   # 最后一份才是游戏里显示的 | The last one is what the game displays
            if new is None or new == cur[-1]: continue
            n += sum(s.set(f, key, lambda _t, v=new: v) for f in FILES)
    return n
