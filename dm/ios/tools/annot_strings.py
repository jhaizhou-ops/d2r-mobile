#!/usr/bin/env python3
"""Genera notas de objetos, runas, zonas y santuarios desde las tablas del juego.

El proceso original de chino simplificado se conserva. Las columnas esES/esMX
usan nombres oficiales y etiquetas españolas, con los mismos datos numéricos.
Las tablas opcionales oldnames.tsv y comments.tsv solo afectan al chino; si no
están presentes, las demás notas siguen disponibles. El resto de idiomas y el
formato de los archivos permanecen intactos.
"""
import csv, hashlib, io, json, os, re, sys
import touch_layout

HERE = os.path.dirname(os.path.abspath(__file__))
STR = '/lng/strings/'


class Strings:
    """Edita únicamente la columna elegida y conserva el formato del archivo."""
    def __init__(self, romfs, repl, lang='zhCN'):
        if lang not in ('zhCN', 'esES', 'esMX'):
            raise ValueError('Idioma de anotaciones no admitido: ' + lang)
        self.lang = lang
        self.romfs, self.repl, self.text, self.path, self.dirty = romfs, repl, {}, {}, set()
        self.original = {}

    @property
    def spanish(self):
        return self.lang in ('esES', 'esMX')

    def _load(self, name):
        if name in self.text: return
        path, text = touch_layout.read_layout(self.romfs, None, suffix=STR + name)
        self.original[name] = {r['Key']: r for r in json.loads(text)}
        prev = self.repl.get(path.lower())   # 前面的步骤（touch_strings）已经换过这份就在它的结果上改 | If an earlier step (touch_strings) has already replaced this file, modify on top of its result
        if prev: text = open(prev, encoding='utf-8-sig').read()
        self.text[name], self.path[name] = text, path

    def _iter(self, name, key):
        self._load(name)
        return list(re.finditer(r'"Key":\s*"%s",.*?"%s":\s*("(?:[^"\\]|\\.)*")' % (re.escape(key.replace('"', '\\"')), self.lang), self.text[name], re.S))

    def has(self, name, key): return bool(self._iter(name, key))

    def keys(self, name):
        self._load(name)
        return re.findall(r'"Key":\s*"((?:[^"\\]|\\.)*)"', self.text[name])

    def get(self, name, key):
        import json
        m = self._iter(name, key)
        return json.loads(m[0].group(1)) if m else None

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
            value = fn(json.loads(m.group(1)))
            if self.spanish and name in ITEM_FILES:
                value = spanish_annotated_name(self.original[name][key][self.lang], value)
            t = t[:m.start(1)] + json.dumps(value, ensure_ascii=False) + t[m.end(1):]
        if ms: self.text[name] = t; self.dirty.add(name)
        return len(ms)

    def write(self, outdir):
        out = {}
        for name in sorted(self.dirty):
            o = os.path.join(outdir, 'annot_' + name)
            open(o, 'w', encoding='utf-8-sig', newline='').write(self.text[name]); out[self.path[name].lower()] = o
        return out



def annotation_lines(text, width=60):
    """Ajusta el texto español; el motor dibuja las líneas de abajo arriba."""
    from textwrap import wrap
    return '\n'.join(reversed(wrap(text, width, break_long_words=False, break_on_hyphens=False)))

def excel(romfs, name):
    _, text = touch_layout.read_layout(romfs, None, suffix='/global/excel/' + name)
    return list(csv.DictReader(io.StringIO(text), delimiter='\t'))


# 物品名的键可能同时出现在 chinese-overlay.json（它后加载、同键覆盖），两处都改
# An item name key may also appear in chinese-overlay.json (it loads later and overrides the same key); change both places
ITEM_FILES = ('item-names.json', 'chinese-overlay.json')


def spanish_annotated_name(original, annotated):
    """Conserva el prefijo gramatical que procesa main+0x41a520/0x41c724."""
    if annotated == original:
        return annotated
    tags = re.findall(r'\[[mf][sp]\]', original)
    if not tags:
        return annotated
    if len(tags) != 1 or not original.startswith(tags[0]) or '[' in original[4:] or ']' in original[4:]:
        raise ValueError('Nombre anotado con gramática original no admitida: ' + original)
    tag = tags[0]
    if annotated.count(tag) != 1 or re.findall(r'\[[mf][sp]\]', annotated) != tags:
        raise ValueError('La anotación no conserva el marcador gramatical: ' + original)
    body = annotated.replace(tag, '', 1)
    body = re.sub(r'(?<=\S)\[', ' [', body).replace('[', '(').replace(']', ')')
    return tag + body


def spanish_class_skill_template(romfs, outdir, repl):
    """Las ocho clases ya llevan paréntesis; elimina la duplicación en esES."""
    path, text = touch_layout.read_layout(str(romfs), None, suffix=STR + 'item-modifiers.json')
    if path.lower() in repl:
        with open(repl[path.lower()], encoding='utf-8-sig') as f:
            text = f.read()
    rows = {r['Key']: r for r in json.loads(text)}
    keys = ('AmaOnly', 'SorOnly', 'NecOnly', 'PalOnly', 'BarOnly', 'DruOnly', 'AssOnly', 'WarOnly')
    assert all(rows[k]['esES'].startswith('(') and rows[k]['esES'].endswith(')') for k in keys)
    assert rows['ItemModifierClassSkill']['esES'] == '%+d a %s (%s)'
    pattern = r'("Key":\s*"ItemModifierClassSkill",.*?"esES":\s*)"%\+d a %s \(%s\)"'
    text, count = re.subn(pattern, lambda m: m[1] + json.dumps('%+d a %s %s'), text, flags=re.S)
    assert count == 1
    output = os.path.join(outdir, 'spanish_item_modifiers.json')
    with open(output, 'w', encoding='utf-8-sig', newline='') as f:
        f.write(text)
    return {path.lower(): output}


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
    # Las traducciones antiguas chinas no sustituyen los nombres oficiales españoles.
    if s.spanish:
        return 0
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
    Rune name = Chinese short name + the original game's English name + number: r30 → 「贝 Ber #30」(Chinese short name, Ber, #30). The inventory description and the ground label both use this key;
    the Chinese comes from the short name rNNL in the runeword recipe (old translation already restored), the English from the enUS column of rNNL."""
    n = 0
    for i in range(1, 34):
        if s.spanish:
            key, short = 'r%02d' % i, 'r%02dL' % i
            name, letter = s.get('item-runes.json', key), s.get('item-runes.json', short)
            if not name or not letter:
                raise SystemExit('annot_strings: falta nombre oficial %s de la runa %d' % (s.lang, i))
            n += s.set('item-runes.json', key, lambda _, name=name, i=i: '%s #%d' % (name, i))
            n += s.set('item-runes.json', short, lambda _, letter=letter, i=i: '%s#%d ' % (letter, i))
            continue
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


def base_tiers(s, romfs):
    """底材名后标档次：普 / 扩 / 精 = 该行 code 等于同行 normcode / ubercode / ultracode；
    身甲与盾类再标轻 / 中 / 重 = armor.txt 的 speed 列 0 / 5 / 10（跑步减速档）。例「神圣铠甲[精·中]」。
    Mark the tier after the base name: 「普 / 扩 / 精」(normal / exceptional / elite) = the row's code equals the same row's normcode / ubercode / ultracode;
    body armor and shields additionally get 「轻 / 中 / 重」(light / medium / heavy) = 0 / 5 / 10 in the speed column of armor.txt (run slowdown tier). Example: 「神圣铠甲[精·中]」(Sacred Armor [elite, medium])."""
    types = {r['Code']: r for r in excel(romfs, 'itemtypes.txt') if r.get('Code')}
    n = 0
    for tab in ('weapons.txt', 'armor.txt'):
        for r in excel(romfs, tab):
            c = r.get('code')
            if not c or r.get('spawnable') != '1' or r.get('quest') not in ('', '0') or r.get('type') == 'tpot': continue
            tier_index = 0 if c == r['normcode'] else 1 if c == r['ubercode'] else 2 if c == r['ultracode'] else None
            tier = (('normal', 'excep.', 'élite') if s.spanish else ('普', '扩', '精'))[tier_index] if tier_index is not None else None
            if not tier: continue
            if tab == 'armor.txt' and (r['type'] == 'tors' or is_shield(r['type'], types)):
                tier += '·' + ({'0': 'ligera', '5': 'media', '10': 'pesada'} if s.spanish else {'0': '轻', '5': '中', '10': '重'})[r['speed']]
            key = r['namestr'] or c
            for f in ITEM_FILES:
                n += s.set(f, key, lambda t, tier=tier: '%s[%s]' % (t, tier))
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
    # La tabla opcional de comentarios contiene solo chino y no se reutiliza en español.
    if s.spanish:
        return 0
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
    All steps are applied in order to the same Strings; returns {step: number of changes}. Old translated names come first (later annotations attach to the restored names);
    drop names come last (miscellaneous items are shortened / colored / given descriptions by the rule table, after old translated names and rune numbers)."""
    from annot_ranges import ann_ranges
    from annot_world import ann_levels, ann_shrines
    from annot_drop import ann_drop
    return {'旧译名': old_names(s), '可变范围': ann_ranges(s, romfs), '点评': comments(s), '符文序号': rune_numbers(s), '底材档次': base_tiers(s, romfs),
            '区域等级': ann_levels(s, romfs), '神殿效果': ann_shrines(s, romfs),
            '掉落名': ann_drop(s, romfs)}


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
COLLECTION_TABLE = 'dm_collection_names.tsv'
TEXTBOX_STEPS = ('旧译名', '符文序号', '底材档次', '区域等级')
_last_table = None


def collection_native_text(text):
    """Recorte original de ChronicleItemListItem, main+b04c3c…b04c7c."""
    raw=text.encode('utf-8');end=raw.rfind(b']')
    return raw[end+1:].decode('utf-8') if end>=3 and raw[end-3:end-2]==b'[' else text


def collection_presentation_pairs(official,current):
    table={}
    for key,source in official.items():
        if not source or key not in current:continue
        name=re.sub(r'^\[(?:ms|fs|mp|fp|ns|np)\]','',source,count=1)
        text=current[key]
        for value in (text,collection_native_text(text)):
            value=value.replace('\u00ffc','\ue07e')
            if not value or value==name:continue
            if value in table and table[value]!=name:
                raise ValueError('Nombre de colección ambiguo para %s: %r' % (key,value))
            table[value]=name
    return table


def collection_presentation_map(romfs,repl):
    import json
    from annot_drop import FILES
    result={}
    for lang in ('esES','esMX'):
        original,current={},{}
        for name in FILES:
            path,text=touch_layout.read_layout(romfs,None,suffix=STR+name)
            original.update((r['Key'],r[lang]) for r in json.loads(text) if lang in r)
            replacement=repl.get(path.lower())
            if replacement:
                with open(replacement,encoding='utf-8-sig') as f:text=f.read()
            current.update((r['Key'],r[lang]) for r in json.loads(text) if lang in r)
        for text,name in collection_presentation_pairs(original,current).items():
            if text in result and result[text]!=name:raise ValueError('Nombre de colección incompatible entre idiomas: '+text)
            result[text]=name
    return result


def write_collection_names(romfs,dst,repl):
    table=collection_presentation_map(romfs,repl)
    esc=lambda t:t.replace('\\','\\\\').replace('\n','\\n').replace('\t','\\t')
    with open(os.path.join(dst,COLLECTION_TABLE),'w',encoding='utf-8',newline='\n') as f:
        f.write('# Texto exacto\tnombre oficial; sólo ItemSlotText de colección PC española\n')
        for text in sorted(table):f.write('%s\t%s\n'%(esc(text),esc(table[text])))
    return len(table)


def apply_textbox(s, romfs):
    from annot_world import ann_levels
    steps = {'旧译名': lambda: old_names(s), '符文序号': lambda: rune_numbers(s), '底材档次': lambda: base_tiers(s, romfs), '区域等级': lambda: ann_levels(s, romfs)}
    return {k: steps[k]() for k in TEXTBOX_STEPS}


def textbox_table(full, plain):
    """full / plain：同一组文件、同一组键的两份 Strings。返回 {运行时注释版: 运行时文字框版}；同一注释版对到两种文字框版 = 生成器自相矛盾，报错。 | full / plain: two Strings over the same set of files and the same set of keys. Returns {runtime annotated version: runtime text-box version}; the same annotated version mapping to two different text-box versions = the generator contradicts itself, raise an error."""
    rt = lambda t: t.replace('\u00ffc', '\ue07e')
    # 同一个键在几份表里都有时，后加载的覆盖前面的（同 annot_drop：最后一份才是游戏里显示的）：先按加载顺序取每个键的生效值
    # When the same key exists in several tables, the later-loaded one overrides the earlier ones (same as annot_drop: the last one is what the game displays): first take each key's effective value in load order
    from annot_drop import FILES as LOAD_ORDER
    order = [n for n in LOAD_ORDER if n in full.dirty] + sorted(n for n in full.dirty if n not in LOAD_ORDER)
    eff = {}
    for name in order:
        for key in full.keys(name):
            a, b = full.get(name, key), plain.get(name, key)
            if a is not None and b is not None: eff[(name if name not in LOAD_ORDER else '*', key)] = (a, b)
    tab, src = {}, {}
    for k, (a, b) in eff.items():
        if 'ÿc' not in a or a == b: continue
        ra, rb = rt(a), rt(b)
        if tab.get(ra, rb) != rb: raise SystemExit('annot_strings：注释版 %r 对到两种文字框版 %r（%s）/ %r（%s）' % (a, tab[ra], src[ra], rb, k))
        tab[ra] = rb; src[ra] = k
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
    _last_table = textbox_table(s, p)
    for lang in ('esES', 'esMX'):
        s.lang = p.lang = lang
        localized = apply_all(s, romfs)
        apply_textbox(p, romfs)
        table = textbox_table(s, p)
        for annotated, plain in table.items():
            if annotated in _last_table and _last_table[annotated] != plain:
                raise SystemExit('annot_strings: equivalencia de texto incompatible entre idiomas: %r' % annotated)
        _last_table.update(table)
        stat[lang] = localized
    stat['文字框对照'] = len(_last_table)
    return s.write(outdir), stat

if __name__ == '__main__':   # 单独跑：python3 annot_strings.py <RomFS> <输出目录> → 打印统计 | Standalone run: python3 annot_strings.py <RomFS> <output dir> → prints statistics
    import sys
    os.makedirs(sys.argv[2], exist_ok=True)
    r, st = build(sys.argv[1], sys.argv[2], {})
    print(st); [print(k, '→', v) for k, v in r.items()]
