#!/usr/bin/env python3
"""Genera las cadenas añadidas a la interfaz desde dm/i18n/ui_text.tsv.

Las columnas originales de chino e inglés se conservan. esES y esMX usan
Español; los demás idiomas mantienen la política de traducción del upstream.
rows() conserva sus triples (clave, chino, inglés) para los generadores existentes.
spanish_rows() expone las traducciones españolas por clave.
"""
import csv, json, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, '..', '..', 'i18n', 'ui_text.tsv')
LANGS = ('enUS', 'zhTW', 'deDE', 'esES', 'frFR', 'itIT', 'koKR', 'plPL', 'esMX', 'jaJP', 'ptBR', 'ruRU', 'zhCN')   # 原版条目的列序 | column order of the original entries
ID0 = 300000   # 原版全部字符串表最大 id 251780；我们的条目从这里起按表序编号 | the largest id across all original string tables is 251780; our entries are numbered from here in table order

PROPER = '老机型补充包|D2R Mobile Resources|D2R Mobile'   # 英文里唯一许可的中文：App 显示名与资源文件夹名（玩家在桌面 /「文件」里看到的就是这几个字，改名另案，见盘点待定 6、7） | the only Chinese allowed in English text: the App display name and the asset folder name (these are the very characters the player sees on the home screen / in "Files"; renaming is a separate matter, see open items 6 and 7 of the inventory)
FMT = re.compile(r'%[-+ #0-9.]*(?:ll|l|h|z)?[a-zA-Z@%]')   # printf / strftime / NSString 占位符：中英两列必须同序同类（运行时拿英文串当格式串用） | printf / strftime / NSString placeholders: the Chinese and English columns must match in order and type (at run time the English string is used as a format string)

# 英文字数闸（进行中页「This Rift will be lost」被截断出框，靠人眼看图没拦住）：大秘境面板 / 秘境提示各栏的英文上限（字符数，占位符按 3 个字符算）。
# 数是按 Mac 截图量出来的每栏像素宽 ÷ 该栏字号下英文每字符平均宽度、再留一个字符余量——是按字数的近似，不是按字形逐个量宽；
# 全是窄字母（i、l、1）的串实际更短，全是宽字母（M、W）的会更长，所以新词进表后仍要出图看一眼
# English length gate (on the in-progress page "This Rift will be lost" was truncated and ran out of its box, and looking at screenshots by eye did not
# catch it): English limits for each field of the Greater Rift panel / Rift messages (in characters, with a placeholder counted as 3 characters).
# The numbers are each field's pixel width measured from Mac screenshots ÷ the average English character width at that field's font size, less one
# character of margin. This is an approximation by character count, not a glyph-by-glyph width measurement;
# a string made of narrow letters only (i, l, 1) is actually shorter and one made of wide letters only (M, W) is longer, so new wording added to the
# table still needs a look at a screenshot
EN_LIMITS = (
    (r'dmRiftSay(?!Prefix).*', 31),
    (r'dmRiftPtsGot', 62), (r'dmRiftPtsHeld', 33),        # 通关点数两句：第一句中文就占两行（提示框自动换行），英文同样两行、不缩写；第二句一行                       # 系统提示：前缀「Rift  」6 个字符 + 正文，提示框一行约 37 个字符 | the two clear-points sentences: the first already takes two lines in Chinese (the message box wraps automatically), so English is likewise two lines, unabbreviated; the second is one line                       # system messages: the prefix "Rift  " is 6 characters plus the body, and one message box line holds about 37 characters
    (r'dmRift(Range|Best|Str|Lvl|Time)', 13),             # 左栏「标签 + 值」的标签（英文专用小字号控件，值最长 7 个字符） | labels of the left column's "label + value" rows (English-only small-font widget; values are at most 7 characters)
    (r'dmRiftGam(Uni|Set)', 8), (r'dmRiftGamAfterEn', 17),
    (r'dmRift(SelT|InfT|GamT|TitleRun|TitleDone|TitleLate)', 16),   # 左栏小标题（中号字，栏宽约 160 像素） | left column small headings (medium font, column about 160 pixels wide)
    (r'dmRift(GuardDown|NextOpen|Saved|RetryOpen|NotCounted)', 16),            # 已完成页那三行（整行，中号字） | the three lines on the completed page (full lines, medium font)
    (r'dmRiftCap.*', 20),                                 # 按钮上方的说明（英文专用小字号控件） | caption above the buttons (English-only small-font widget)
    (r'dmRiftBtn.*', 12),                                 # 按钮上的字 | text on the buttons
    (r'dmRiftMsItem\d+', 15), (r'dmRiftMsSub\d+', 19), (r'dmRiftMsTier\d+', 4), (r'dmRiftMs(Got|Next|No)', 7), (r'dmRiftMsT', 12), (r'dmRiftMsNEn', 38),
    (r'dmRiftHud(Title)', 4), (r'dmRiftHud(Done|Late|Guard)', 18), (r'dmRiftHud(Next|Same|Kill|Goal)', 26),
    (r'dmKbRiftUnlock', 9), (r'dmKbRift(Best|Total)', 16),
)
# 键只许字母数字：布局里「@键」的解析遇到 _ 就断（@dmCustom_auto_mana 显示成「丢失字符串_auto_mana」，Mac 上的 iOS 包实测）
# 英文界面下也要写汉字的键（自定义页语言那一行：切错语言的人得看得懂）。只许用在指定了全球字体 BlizzardGlobal 的文字控件上——英文界面缺省字体没有汉字
# Keys may only contain letters and digits: the layout's "@key" parsing stops at _ (@dmCustom_auto_mana was shown as "missing string_auto_mana", measured in the iOS package on a Mac)
# Keys that must show Chinese characters in the English UI as well (the language row of the Custom page: someone who switched to the wrong language has to be able to read it). Only allowed on text controls that specify the global font BlizzardGlobal -- the default font of the English UI has no Chinese characters
EN_HAS_ZH = {'dmCustomLang', 'dmCustomLangZh', 'dmCustomLangHintZh'}
# Las cotas españolas heredan las columnas estrechas del upstream. Los tres
# nombres oficiales más largos necesitan ajuste de fuente en la maquetación.
ES_LIMITS = (
    (r'dmRiftMsItem30', 26),
    (r'dmRiftMsItem60', 24),
    (r'dmRiftMsItem80', 17),
    (r'dmRiftHudTitle', 5),
) + EN_LIMITS

# Igualdad deliberada, nunca un idioma de reserva implícito. La comparación con
# el valor exacto impide que añadir una clave aquí oculte una frase sin traducir.
ES_SHARED_TEXT = {
    'dmBrAndariel': 'Andariel', 'dmBrDuriel': 'Duriel',
    'dmBrDiablo': 'Diablo', 'dmBrBaal': 'Baal',
    'dmDlAppFolder': 'D2R Mobile', 'dmRvBytes': '%lld bytes',
    'dmDlEtc': ' etc.', 'dmRiftMsItem20': 'Annihilus',
    'dmCustomLangZh': '中文', 'dmCustomLangEnOpt': 'English',
    'dmCustomLangEs': 'Español',
    'dmCustomLangHintEs': 'Pulsa Aplicar y reinicia el juego.',
    'dmCustomLangHintZh': '点「应用」，然后把 App 关掉重开',
    'dmCustomLangHintEn': 'Tap Apply, then close and reopen the app',
}
ES_HAS_ZH = {'dmCustomLangZh', 'dmCustomLangHintZh'}


def _localized_rows():
    out = []
    with open(TABLE, encoding='utf-8', newline='') as f:
        reader = csv.DictReader(f, delimiter='\t')
        required = {'键', '中文', 'English', 'Español'}
        if not required.issubset(reader.fieldnames or ()):
            raise SystemExit('dm_i18n: faltan columnas obligatorias: %s' %
                             sorted(required - set(reader.fieldnames or ())))
        for row in reader:
            k = row['键']
            values = [row[name].replace('\\n', '\n') if row.get(name) is not None else ''
                      for name in ('中文', 'English', 'Español')]
            zh, en, es = values
            if (None in row or not re.fullmatch(r'dm[A-Za-z0-9]+', k)
                    or any(not value.strip() for value in values)):
                raise SystemExit('dm_i18n: fila incompleta o inválida: %r' % row)
            for value, label, allowed in ((en, 'inglés', EN_HAS_ZH),
                                           (es, 'español', ES_HAS_ZH)):
                if k not in allowed and re.search(r'[　-鿿＀-￯]', re.sub(PROPER, '', value)):
                    raise SystemExit('dm_i18n: %s contiene caracteres chinos en %s: %r' %
                                     (k, label, value))
                if FMT.findall(zh) != FMT.findall(value):
                    raise SystemExit('dm_i18n: %s cambia el orden o tipo de marcadores en %s: %s / %s' %
                                     (k, label, FMT.findall(zh), FMT.findall(value)))
            if '\\\n' in es:
                raise SystemExit('dm_i18n: %s tiene un salto de línea español con doble barra' % k)
            # Los nombres, códigos, separadores y formatos puros pueden coincidir.
            if es == en and re.search(r'[^\W\d_]', FMT.sub('', es), re.UNICODE):
                if ES_SHARED_TEXT.get(k) != es:
                    raise SystemExit('dm_i18n: %s conserva el inglés sin traducción española: %r' % (k, es))
            for value, label, limits in ((en, 'inglés', EN_LIMITS),
                                         (es, 'español', ES_LIMITS)):
                limit = next((n for pattern, n in limits if re.fullmatch(pattern, k)), None)
                # El upstream medía solo la primera línea inglesa. En español
                # comprobamos todas las líneas que puede mostrar el control.
                lines = FMT.sub('000', value).split('\n')
                length = max(map(len, lines if label == 'español' else lines[:1]))
                if limit is not None and length > limit:
                    raise SystemExit('dm_i18n: %s, %s: %d caracteres; límite %d: %r' %
                                     (k, label, length, limit, value))
            out.append((k, zh, en, es))
    if not out:
        raise SystemExit('dm_i18n: la tabla de traducciones está vacía')
    keys = [r[0] for r in out]
    if len(set(keys)) != len(keys):
        raise SystemExit('dm_i18n: claves duplicadas: %s' %
                         sorted({k for k in keys if keys.count(k) > 1}))
    return out


def rows():
    """Compatibilidad con los generadores que consumen triples chino/inglés."""
    return [(k, zh, en) for k, zh, en, _ in _localized_rows()]


def spanish_rows():
    """Traducciones españolas completas; un texto ausente es un error."""
    return {k: es for k, _, _, es in _localized_rows()}

def build(romfs, outdir, repl):
    """repl：前面步骤已换的 {包内逻辑路径: 本地文件}。返回 ({包内逻辑路径: 本地文件}, 条数)。 | repl: {logical path in the package: local file} already replaced by earlier steps. Returns ({logical path in the package: local file}, entry count)."""
    import touch_layout   # 用时再导：touch_layout 自己在模块层要读本表（自定义页行数），顶层互导会半初始化 | imported at use time: touch_layout itself reads this table at module level (row count of the Custom page), and a top-level mutual import would leave it half-initialized
    path, text = touch_layout.read_layout(romfs, None, suffix='/lng/strings/ui.json')
    prev = repl.get(path.lower())
    if prev: text = open(prev, encoding='utf-8-sig').read()
    have = set(re.findall(r'"Key":\s*"((?:[^"\\]|\\.)*)"', text))
    rs = _localized_rows()
    clash = [k for k, _, _, _ in rs if k in have]
    if clash: raise SystemExit('dm_i18n：键和原版重名：%s' % clash)
    add = [{'id': ID0 + i, 'Key': k, **{l: (zh if l in ('zhCN', 'zhTW') else es if l in ('esES', 'esMX') else en) for l in LANGS}} for i, (k, zh, en, es) in enumerate(rs)]
    end = text.rstrip().rindex(']')
    body = ',\n'.join('  ' + json.dumps(e, ensure_ascii=False) for e in add)
    text = text[:end].rstrip() + ',\n' + body + '\n' + text[end:]
    json.loads(text)   # 拼完还是合法 JSON | still valid JSON after assembly
    o = os.path.join(outdir, 'i18n_ui.json'); open(o, 'w', encoding='utf-8-sig', newline='').write(text)
    return {path.lower(): o}, len(add)

def header():
    """dm/host/dm_i18n_tab.h 的内容（游戏外画的字用的编译期表）。 | Contents of dm/host/dm_i18n_tab.h (the compile-time table for text drawn outside the game)."""
    c = lambda s: json.dumps(s, ensure_ascii=False)   # JSON 串的转义在 C 里同样成立（\n \" \\） | JSON string escapes are equally valid in C (newline, double quote, backslash)
    out = ['// Archivo generado desde dm/i18n/ui_text.tsv: python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h', '#pragma once',
           'static const struct { const char *k, *zh, *en, *es; } k_dm_i18n[] = {']
    out += ['    { %s, %s, %s, %s },' % (c(k), c(zh), c(en), c(es)) for k, zh, en, es in _localized_rows()]
    return '\n'.join(out + ['};', ''])

if __name__ == '__main__':
    import sys
    if sys.argv[1:] == ['--header']: sys.stdout.write(header())
    elif sys.argv[1:] == ['--check']:   # 单测：表与入库的头文件一致 | unit test: the table matches the checked-in header file
        h = os.path.join(HERE, '..', '..', 'host', 'dm_i18n_tab.h')
        if open(h, encoding='utf-8').read() != header(): raise SystemExit('dm_i18n：dm/host/dm_i18n_tab.h 过期，重跑 python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h')
        print('dm_i18n OK: %d/%d cadenas en español; marcadores y longitudes válidos' % (len(spanish_rows()), len(rows())))
    else: raise SystemExit('用法：dm_i18n.py --header | --check')
