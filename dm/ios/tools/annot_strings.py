#!/usr/bin/env python3
"""物品 / 符文 / 区域 / 怪物 / 神殿的注释与旧译名（注释要、旧译名整体换回、不加英文缩写）。
做法 = RomFS 覆盖层换字符串文件（与 touch_strings 同一套机制），按键名定位、原文不符即报错。
改 zhCN、esES、esMX 三列（LANGS），每列各算一遍、各写各的列，名字取该列的现行值；旧译名、点评是中文特有内容，只做 zhCN。
注释的数字一律从我们 3.3 包里的 excel 表算（不拷红叶的表）；旧译名表 annot/oldnames.tsv 是从红叶 mod 的名字部分抽出、
逐条与 3.3 原文对过的。
Annotations and old names for items / runes / areas / monsters / shrines (annotations wanted, old names restored wholesale, no English abbreviations added).
Method = RomFS overlay replaces string files (same mechanism as touch_strings), located by key name, error if the original text doesn't match.
Changes the zhCN, esES, esMX columns (LANGS), computed separately and written to its own column each, names from that column's current value; old names and commentary are Chinese-specific, done only for zhCN.
Annotation numbers are always computed from the excel tables in our 3.3 package (Hongye's tables are not copied); the old-name table annot/oldnames.tsv was extracted from the name part of the Hongye mod
and checked entry by entry against the 3.3 original text."""
import csv, hashlib, io, os, re, sys
import touch_layout

HERE = os.path.dirname(os.path.abspath(__file__))
STR = '/lng/strings/'


LANGS = ('zhCN', 'esES', 'esMX')   # 注释写进的语言列；先做 zhCN（中文列的产出与只做中文时逐字节一致） | Language columns annotations are written into; zhCN goes first (Chinese column output is byte-identical to Chinese-only)
# 西语名字串开头的语法性别标记（[fs] 阴单、[ms] 阳单、[fp]、[mp]），游戏据它配魔法词缀的阴阳性；改写只动标记后面的部分，标记留在串首
# Grammatical gender marker at the start of Spanish name strings ([fs] fem. sing., [ms] masc. sing., [fp], [mp]); the game uses it to match the gender of magic affixes; rewrites only touch the part after the marker, the marker stays at the start
GENDER_TAG = re.compile(r'(?:\[[fm][sp]\])+')


class Strings:
    """字符串文件的按键改写：保留原文件排版，只换当前语言列 col 的值（缺省 zhCN）。 | Keyed rewrite of a string file: keep the original file layout, replace only the value of the current language column col (default zhCN)."""
    def __init__(self, romfs, repl):
        self.romfs, self.repl, self.text, self.path, self.dirty = romfs, repl, {}, {}, set()
        self.col = 'zhCN'

    def _skip(self, name):
        """chinese-overlay.json 只在中文下加载（它的其余语言列是英文占位，119 条里 118 条 esES = enUS）：非中文列当作没有这份。 | chinese-overlay.json is only loaded in Chinese (its other language columns are English placeholders, 118 of 119 entries have esES = enUS): treat non-Chinese columns as if this file didn't exist."""
        return self.col != 'zhCN' and name == 'chinese-overlay.json'

    def _split(self, v):
        """(性别标记, 其余)；中文列不拆。 | (gender marker, rest); Chinese columns are not split."""
        m = GENDER_TAG.match(v) if self.col != 'zhCN' else None
        return (m.group(), v[m.end():]) if m else ('', v)

    def _load(self, name):
        if name in self.text: return
        path, text = touch_layout.read_layout(self.romfs, None, suffix=STR + name)
        prev = self.repl.get(path.lower())   # 前面的步骤（touch_strings）已经换过这份就在它的结果上改 | If an earlier step (touch_strings) has already replaced this file, modify on top of its result
        if prev: text = open(prev, encoding='utf-8-sig').read()
        self.text[name], self.path[name] = text, path

    def _iter(self, name, key):
        if self._skip(name): return []
        self._load(name)
        return list(re.finditer(r'"Key":\s*"%s",.*?"%s":\s*("(?:[^"\\]|\\.)*")' % (re.escape(key.replace('"', '\\"')), self.col), self.text[name], re.S))

    def has(self, name, key): return bool(self._iter(name, key))

    def keys(self, name):
        self._load(name)
        return re.findall(r'"Key":\s*"((?:[^"\\]|\\.)*)"', self.text[name])

    def get(self, name, key):
        import json
        m = self._iter(name, key)
        return self._split(json.loads(m[0].group(1)))[1] if m else None

    def get_lang(self, name, key, lang):
        """同一条目里别的语言列（如 enUS）。 | Other language columns in the same entry (e.g. enUS)."""
        import json
        self._load(name)
        m = re.search(r'"Key":\s*"%s",.*?"%s":\s*("(?:[^"\\]|\\.)*")' % (re.escape(key), lang), self.text[name], re.S)
        return json.loads(m.group(1)) if m else None

    def set(self, name, key, fn):
        """fn(原文) → 新文；同一文件里同键多处一起改。返回改了几处。 | fn(original text) → new text; multiple occurrences of the same key in one file are changed together. Returns the number of changes."""
        import json
        ms = self._iter(name, key)
        t = self.text[name]
        for m in reversed(ms):
            tag, v = self._split(json.loads(m.group(1)))
            nv = fn(v)
            # 西语列：注释一律用圆括号。串里还有别的方括号时，游戏解析串首性别标记（[fp] 等）失败、把标记原样画出来
            # （西语截图：编年史里「[fp]*Garfas enormes [Exc]」）；中文列照旧
            # Spanish columns: annotations always use parentheses. When the string contains other square brackets, the game fails to parse the leading gender marker ([fp] etc.) and draws it literally
            # (Spanish screenshot: "[fp]*Garfas enormes [Exc]" in the chronicle); Chinese columns unchanged
            if self.col != 'zhCN': nv = nv.replace('[', '(').replace(']', ')')
            t = t[:m.start(1)] + json.dumps(tag + nv, ensure_ascii=False) + t[m.end(1):]
        if ms: self.text[name] = t; self.dirty.add(name)
        return len(ms)

    def write(self, outdir):
        out = {}
        for name in sorted(self.dirty):
            o = os.path.join(outdir, 'annot_' + name)
            open(o, 'w', encoding='utf-8-sig', newline='').write(self.text[name]); out[self.path[name].lower()] = o
        return out


def excel(romfs, name):
    _, text = touch_layout.read_layout(romfs, None, suffix='/global/excel/' + name)
    return list(csv.DictReader(io.StringIO(text), delimiter='\t'))


# 物品名的键可能同时出现在 chinese-overlay.json（它后加载、同键覆盖），两处都改
# An item name key may also appear in chinese-overlay.json (it loads later and overrides the same key); change both places
ITEM_FILES = ('item-names.json', 'chinese-overlay.json')


def text_hash(t):
    """原文校验：游戏现行文字的 SHA-1 前 8 位（表里不存原文，只存这个，防原包换版本后照旧改）。 | Source-text check: first 8 characters of the SHA-1 of the game's current text (the table does not store the source text, only this, to prevent applying the same change after the original package changes version)."""
    return hashlib.sha1(t.encode('utf-8')).hexdigest()[:8]


OLDNAMES = os.path.join(HERE, 'annot', 'oldnames.tsv')
def old_rows():
    """旧译名对照表的各行。表可以没有：公开仓库不带这份表（它是成套的他人译名），没有时「换回旧译名」这一步什么都不做，
    注释、点评、符文序号等其余步骤照常。自备一份放到 annot/oldnames.tsv 即启用，格式见该目录下的 README。
    Rows of the old-name table. The table may be absent: the public repository does not carry it (it is a complete set of someone else's translated names); without it the "switch back to old translated names" step does nothing,
    and the other steps (annotations, comments, rune numbers, etc.) run as usual. Put your own copy at annot/oldnames.tsv to enable it; for the format see the README in that directory."""
    if not os.path.isfile(OLDNAMES):
        if not getattr(old_rows, 'said', 0): old_rows.said = 1; print('annot_strings：没有旧译名对照表 %s，不换旧译名（其余注释照常）' % OLDNAMES, file=sys.stderr)
        return []
    return list(csv.DictReader(open(OLDNAMES, encoding='utf-8'), delimiter='\t'))


def old_names(s):
    n = 0
    for row in old_rows():
        f, k = row['文件'] + '.json', row['键']
        new = row['旧译名'].replace('\\n', '\n')
        cur = s.get(f, k)
        if cur is None: raise SystemExit('annot_strings：%s 里没有键 %s' % (f, k))
        if text_hash(cur) != row['原文校验']: raise SystemExit('annot_strings：%s / %s 的原文变了（校验 %s，现行「%s」）——原包换了版本，核过再改表' % (f, k, row['原文校验'], cur))
        n += s.set(f, k, lambda _: new)
    return n


def rune_numbers(s):
    """符文名 = 中文短名 + 原版英文名 + 序号：r30 → 「贝 Ber #30」。背包说明与地上标签都用这个键；
    中文取符文之语配方里的短名 rNNL（已换旧译），英文取 rNNL 的 enUS 列。
    西语列：原版名已是「Runa Ber」（含英文短名），只加序号 → 「Runa Ber #30」；配方短名同样「Ber#30 」。
    Rune name = Chinese short name + original English name + number: r30 → "贝 Ber #30". Used as the key for both inventory descriptions and ground labels;
        Chinese from the short name rNNL in the runeword recipes (old names already applied), English from the enUS column of rNNL.
        Spanish columns: the original name is already "Runa Ber" (including the English short name), only add the number → "Runa Ber #30"; recipe short names likewise "Ber#30 "."""
    n = 0
    if s.col != 'zhCN':
        for i in range(1, 34):
            nm, short = s.get('item-runes.json', 'r%02d' % i), s.get('item-runes.json', 'r%02dL' % i)
            if not nm or not short or not re.fullmatch(r"\S+ %s" % re.escape(short), nm) or not re.fullmatch(r"[A-Za-z']+", short): raise SystemExit('annot_strings：%s 的 r%02d / r%02dL 不是「Runa 短名」/「短名」：%r %r' % (s.col, i, i, nm, short))
            n += s.set('item-runes.json', 'r%02d' % i, lambda _, i=i, nm=nm: '%s #%d' % (nm, i))
            n += s.set('item-runes.json', 'r%02dL' % i, lambda _, i=i, short=short: '%s#%d ' % (short, i))
        return n
    for i in range(1, 34):
        zh, en = s.get('item-runes.json', 'r%02dL' % i), s.get_lang('item-runes.json', 'r%02dL' % i, 'enUS')
        if not zh or not en or not re.fullmatch(r"[A-Za-z']+", en): raise SystemExit('annot_strings：r%02dL 的中 / 英短名不对：%r %r' % (i, zh, en))
        n += s.set('item-runes.json', 'r%02d' % i, lambda _, i=i, zh=zh, en=en: '%s %s #%d' % (zh, en, i))
        # 配方短名也带序号：rNNL 只被 gems.txt 的 letter 列引用，用在符文之语装备说明框的配方行，
        # 游戏把各符文短名直接拼进一对引号（实测「'奈夫提尔'」），所以序号后留一个空格分隔：「'乔#31 伊斯特#24 贝#30 '」
        # Recipe short names also carry the number: rNNL is referenced only by the letter column of gems.txt and is used in the recipe line of the runeword item tooltip;
        # the game concatenates the rune short names directly inside one pair of quotes (measured: 「'奈夫提尔'」('NefTir')), so one space is left after the number as a separator: 「'乔#31 伊斯特#24 贝#30 '」('Jah#31 Ist#24 Ber#30 ')
        n += s.set('item-runes.json', 'r%02dL' % i, lambda _, i=i, zh=zh: '%s#%d ' % (zh, i))
    return n


def is_shield(code, types):
    seen = set()
    while code and code not in seen:
        if code == 'shld': return True
        seen.add(code); code = types.get(code, {}).get('Equiv1', '')
    return False


# 底材档次的措辞：(普 / 扩 / 精, 轻 / 中 / 重, 拼法)。西语档次取原版 NormalQuality / ExceptionalQuality / EliteQuality 的前三个字母
# Wording for base grades: (normal / exceptional / elite, light / medium / heavy, format). Spanish grades take the first three letters of the original NormalQuality / ExceptionalQuality / EliteQuality
TIER_WORDS = {'zhCN': (('普', '扩', '精'), {'0': '轻', '5': '中', '10': '重'}, '%s[%s]'),
              'esES': (('Nor', 'Exc', 'Éli'), {'0': 'lig', '5': 'med', '10': 'pes'}, '%s [%s]'),
              'esMX': (('Nor', 'Exc', 'Éli'), {'0': 'lig', '5': 'med', '10': 'pes'}, '%s [%s]')}


def base_tiers(s, romfs):
    """底材名后标档次：普 / 扩 / 精 = 该行 code 等于同行 normcode / ubercode / ultracode；
    身甲与盾类再标轻 / 中 / 重 = armor.txt 的 speed 列 0 / 5 / 10（跑步减速档）。例「神圣铠甲[精·中]」「Armadura sagrada [Éli·med]」。
    Mark the grade after base names: normal / exceptional / elite = the row's code equals the same row's normcode / ubercode / ultracode;
        body armor and shields also get light / medium / heavy = armor.txt speed column 0 / 5 / 10 (run slowdown tier). E.g. "神圣铠甲[精·中]" "Armadura sagrada [Éli·med]"."""
    (wn, wu, wl), wsp, fmt = TIER_WORDS[s.col]
    types = {r['Code']: r for r in excel(romfs, 'itemtypes.txt') if r.get('Code')}
    n = 0
    for tab in ('weapons.txt', 'armor.txt'):
        for r in excel(romfs, tab):
            c = r.get('code')
            if not c or r.get('spawnable') != '1' or r.get('quest') not in ('', '0') or r.get('type') == 'tpot': continue
            tier = wn if c == r['normcode'] else wu if c == r['ubercode'] else wl if c == r['ultracode'] else None
            if not tier: continue
            if tab == 'armor.txt' and (r['type'] == 'tors' or is_shield(r['type'], types)):
                tier += '·' + wsp[r['speed']]
            key = r['namestr'] or c
            for f in ITEM_FILES:
                n += s.set(f, key, lambda t, tier=tier: fmt % (t, tier))
    return n


def comments(s):
    """暗金 / 套装 / 符文之语的一句点评（我们自己写的，按红叶点评的意思逐条重写、字面不照搬）。绿色一行，显示在名字下面、范围行上面：
    串 = [范围行\n]ÿc2点评\n<名字色>名字（游戏把多行名字倒着画，最后一行在最上面）。
    核对：点评里不许出现「已换回旧译名的技能」的 3.3 现行名（点评用换回后的名字），有就报错。
    A one-sentence review for uniques / sets / runewords (written by ourselves, rewritten entry by entry after the meaning of the Hongye community mod's reviews, wording not copied). One green line, shown below the name and above the range line:
    string = [range line\\
    ]ÿc2review\\
    <name color>name (the game draws multi-line names in reverse, last line on top).
    Check: a review must not contain the 3.3 current name of "a skill already switched back to its old translated name" (reviews use the restored names); raise an error if it does."""
    renamed, orig = {}, Strings(s.romfs, s.repl)   # orig：没改过的那份，取技能现行名 | orig: the unmodified copy, used to get the skills' current names
    for row in old_rows():
        if row['文件'] == 'skills' and row['键'].lower().startswith('skillname'):
            cur = orig.get('skills.json', row['键'])
            if cur and cur != row['旧译名']: renamed[cur] = row['旧译名']
    kept = {s.get('skills.json', k) for k in s.keys('skills.json') if k.lower().startswith('skillname')}   # 换名后仍在用的名字（两个技能同名、只改了一个，如「专注」）不算错 | A name still in use after the rename (two skills shared a name and only one was changed, e.g. 「专注」(Concentration)) is not an error
    renamed = {a: b for a, b in renamed.items() if a not in kept}
    n = 0
    cp = os.path.join(HERE, 'annot', 'comments.tsv')
    if not os.path.isfile(cp):   # 点评表可以没有（公开仓库不带：它是照别人的点评逐条改写的）：没有就不加点评，其余注释照常。自备格式见 annot/README.md | the comments table may be absent (the public repository does not carry it: it was rewritten entry by entry from someone else's comments): without it no comments are added, and the other annotations are produced as usual. For the format of your own copy see annot/README.md
        print('annot_strings：没有点评表 %s，不加点评（其余注释照常）' % cp, file=sys.stderr); return 0
    for row in csv.DictReader(open(cp, encoding='utf-8'), delimiter='\t'):
        c, color = row['点评'], '\u00ffc2' if row['名字颜色'] == 'set' else '\u00ffc4'
        bad = [a for a in renamed if a in c and renamed[a] not in c]
        if bad: raise SystemExit('annot_strings：%s 的点评「%s」里有 3.3 现行技能名 %s（应写旧译名 %s）' % (row['键'], c, bad, [renamed[a] for a in bad]))
        def add(t, c=c, color=color):
            if '\n' in t: head, name = t.rsplit('\n', 1); return '%s\n\u00ffc2%s\n%s' % (head, c, name)
            return '\u00ffc2%s\n%s%s' % (c, color, t)
        files = ('item-runes.json', 'chinese-overlay.json') if row['文件'] == 'item-runes' else ITEM_FILES
        k = sum(s.set(f, row['键'], add) for f in files)
        if not k: raise SystemExit('annot_strings：点评的键 %s 在字符串表里没有' % row['键'])
        n += k
    return n


def apply_all(s, romfs):
    """全部步骤按次序做在同一个 Strings 上；返回 {步骤: 改了几处}。旧译名在前（后面的注释挂在换回后的名字上）；
    掉落名最后（杂物类按规则表缩短 / 加色 / 加说明，在旧译名与符文序号之后）。
    LANGS 每列各做一遍；中文列的统计在顶层，西语列的统计放在 stat[列名] 里。旧译名、点评只做中文列。
    Run all steps in order on the same Strings; returns {step: number of changes}. Old names first (later annotations attach to the restored names);
        drop names last (misc items shortened / colored / annotated per the rule table, after old names and rune numbers).
        Done once per LANGS column; Chinese column stats at top level, Spanish column stats in stat[column name]. Old names and commentary are done only for the Chinese column."""
    from annot_ranges import ann_ranges
    from annot_world import ann_levels, ann_shrines
    from annot_drop import ann_drop
    stat = {}
    for col in LANGS:
        s.col = col; zh = col == 'zhCN'
        st = {'旧译名': old_names(s) if zh else 0, '可变范围': ann_ranges(s, romfs), '点评': comments(s) if zh else 0, '符文序号': rune_numbers(s), '底材档次': base_tiers(s, romfs),
              '区域等级': ann_levels(s, romfs), '神殿效果': ann_shrines(s, romfs),
              '掉落名': ann_drop(s, romfs)}
        if zh: stat.update(st)
        else: stat[col] = {k: v for k, v in st.items() if k not in ('旧译名', '点评')}
    s.col = 'zhCN'
    return stat


# 普通文字框（TextBoxWidget）不认颜色码：游戏加载时把 ÿc 换成私用区字符 U+E07E，文字框原样画成「*5」、换行画成空格
# （真机上编年史列表所见）。说明框（TooltipsPanel）、聊天（ChatListWidget）认。所以除了写进字符串表的注释版，再出一份「文字框版」：
# 同一条流水线只做改名类步骤（旧译名、符文序号、底材档次、区域等级），不做装饰类步骤（可变范围行、点评行、神殿效果行、掉落名的缩写 / 颜色 / 说明行）。
# 两份逐键对照，凡注释版带颜色码且与文字框版不同的，记一行「注释版 → 文字框版」（都换成运行时的 U+E07E 写法）。
# 宿主在文字框写字入口（观测门 14，dm_textbox.c）只按这张表整串 / 子串替换：原版自带的串不在表里、一个字节都不动；注释开关关掉时没有这张表
# Ordinary text boxes (TextBoxWidget) do not understand color codes: at load time the game replaces ÿc with the private-use character U+E07E, and the text box draws it verbatim as "*5" and draws newlines as spaces
# (as seen in the chronicle list on a real device). The tooltip box (TooltipsPanel) and chat (ChatListWidget) do understand them. So besides the annotated version written into the string table, a "text-box version" is also produced:
# the same pipeline does only the renaming steps (old translated names, rune numbers, base tiers, area levels) and not the decorative steps (variable range lines, review lines, shrine effect lines, and the abbreviations / colors / description lines of drop names).
# The two are compared key by key; wherever the annotated version has color codes and differs from the text-box version, one line "annotated version → text-box version" is recorded (both converted to the runtime U+E07E form).
# At the text-box write entry (observation gate 14, dm_textbox.c) the host only does whole-string / substring replacement by this table: the original game's own strings are not in the table and not a single byte of them is touched; when the annotation switch is off this table does not exist
TEXTBOX_TABLE = 'dm_textbox_plain.tsv'
TEXTBOX_STEPS = ('旧译名', '符文序号', '底材档次', '区域等级')
_last_table = None


def apply_textbox(s, romfs):
    from annot_world import ann_levels
    steps = {'旧译名': lambda: old_names(s) if s.col == 'zhCN' else 0, '符文序号': lambda: rune_numbers(s), '底材档次': lambda: base_tiers(s, romfs), '区域等级': lambda: ann_levels(s, romfs)}
    out = {}
    for col in LANGS:
        s.col = col; out[col] = {k: steps[k]() for k in TEXTBOX_STEPS}
    s.col = 'zhCN'
    return out


def textbox_table(full, plain):
    """full / plain：同一组文件、同一组键的两份 Strings。返回 {运行时注释版: 运行时文字框版}；同一注释版对到两种文字框版 = 生成器自相矛盾，报错。 | full / plain: two Strings over the same set of files and the same set of keys. Returns {runtime annotated version: runtime text-box version}; the same annotated version mapping to two different text-box versions = the generator contradicts itself, raise an error."""
    rt = lambda t: t.replace('\u00ffc', '\ue07e')
    # 同一个键在几份表里都有时，后加载的覆盖前面的（同 annot_drop：最后一份才是游戏里显示的）：先按加载顺序取每个键的生效值
    # When the same key exists in several tables, the later-loaded one overrides the earlier ones (same as annot_drop: the last one is what the game displays): first take each key's effective value in load order
    from annot_drop import FILES as LOAD_ORDER
    order = [n for n in LOAD_ORDER if n in full.dirty] + sorted(n for n in full.dirty if n not in LOAD_ORDER)
    tab, src = {}, {}
    for col in LANGS:   # 各语言列各取生效值，并进同一张表（宿主不分语言，按串查） | Take each language column's effective value and merge into one table (the host is language-agnostic, looks up by string)
        full.col = plain.col = col; eff = {}
        for name in order:
            for key in full.keys(name):
                a, b = full.get(name, key), plain.get(name, key)
                if a is not None and b is not None: eff[(name if name not in LOAD_ORDER else '*', key)] = (a, b)
        for k, (a, b) in eff.items():
            if 'ÿc' not in a or a == b: continue
            ra, rb = rt(a), rt(b)
            if tab.get(ra, rb) != rb and src[ra][0] != col:   # 两种语言列写出同一个注释版、文字框版不同（如 esES / esMX 都是「Morada」，原名两列不同）：表不分语言，改用去掉颜色码的注释版 | Two language columns write the same annotated version but different text-box versions (e.g. esES / esMX both "Morada", original names differ between the two columns): the table is language-agnostic, so use the annotated version with color codes stripped instead
                tab[ra] = re.sub('\\ue07e.', '', ra); src[ra] = (src[ra][0] + '/' + col,) + k; continue
            if tab.get(ra, rb) != rb: raise SystemExit('annot_strings：注释版 %r 对到两种文字框版 %r（%s）/ %r（%s %s）' % (a, tab[ra], src[ra], rb, col, k))
            tab[ra] = rb; src[ra] = (col,) + k
    full.col = plain.col = 'zhCN'
    return tab


def write_textbox_table(dst):
    """gen_romfs_overlay 写完带本特性的变体后调：把最近一次 build 算出的对照表写进变体目录（一行一条，键与值里的 \\ / 换行 / 制表转义）。 | Called by gen_romfs_overlay after it writes a variant with this feature: writes the mapping table computed by the most recent build into the variant directory (one entry per line; \\\\ / newline / tab in keys and values are escaped)."""
    if _last_table is None: raise SystemExit('annot_strings：还没 build 就写文字框对照表')
    esc = lambda t: t.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')
    with open(os.path.join(dst, TEXTBOX_TABLE), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# 注释版\t文字框版（annot_strings.py 生成，宿主 dm_textbox.c 读）\n')
        for a in sorted(_last_table): f.write('%s\t%s\n' % (esc(a), esc(_last_table[a])))
    return len(_last_table)


def build(romfs, outdir, repl):
    """repl：前面步骤已换的 {包内逻辑路径: 本地文件}。返回 ({包内逻辑路径: 本地文件}, {步骤: 改了几处})。 | repl: {in-package logical path: local file} already replaced by earlier steps. Returns ({in-package logical path: local file}, {step: number of changes})."""
    global _last_table
    s = Strings(romfs, repl)
    stat = apply_all(s, romfs)
    p = Strings(romfs, repl); apply_textbox(p, romfs)
    _last_table = textbox_table(s, p); stat['文字框对照'] = len(_last_table)
    return s.write(outdir), stat

if __name__ == '__main__':   # 单独跑：python3 annot_strings.py <RomFS> <输出目录> → 打印统计 | Standalone run: python3 annot_strings.py <RomFS> <output dir> → prints statistics
    import sys
    os.makedirs(sys.argv[2], exist_ok=True)
    r, st = build(sys.argv[1], sys.argv[2], {})
    print(st); [print(k, '→', v) for k, v in r.items()]
