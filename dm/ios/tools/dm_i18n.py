#!/usr/bin/env python3
"""我们自己加的界面文字的多语表：唯一来源 dm/i18n/ui_text.tsv（键 / 中文 / English / Español / Español MX / Русский / 用在哪）。
做法 = 把每一行作为新条目追加进原版字符串表 ui.json（RomFS 覆盖层，与 touch_strings 同一套机制）：zhCN 列 = 中文、zhTW 列 = 中文转繁（to_traditional），
esES 列 = Español，esMX 列 = Español MX（空 = 同 Español；只在拉美官方用词与 esES 不同时填），ruRU 列 = Русский，其余 8 种语言列 = English。
这样布局里写「@键」、宿主用原版取串 main+0x7f5630 取，语言切换由原版自己重装字符串表完成（SetLanguage，r34 起不剪），不用我们另跟语言。
'''Multilingual table for UI text we added ourselves: single source dm/i18n/ui_text.tsv (key / Chinese / English / Español / Español MX / Russian / where used).
Method = append each row as a new entry to the original string table ui.json (RomFS overlay, same mechanism as touch_strings): zhCN column = Chinese, zhTW column = Chinese converted to Traditional (to_traditional),
esES column = Español, esMX column = Español MX (empty = same as Español; filled only where official Latin American wording differs from esES), ruRU column = Russian, the other 8 language columns = English.
So layouts write "@key", the host looks up via the original main+0x7f5630, and language switching is done by the original game reloading the string table (SetLanguage, not cut since r34); we don't track language separately.'''"""
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
EN_HAS_ZH = {'dmCustomLangZh', 'dmCustomLangHintZh'}
def rows():
    """[(键, 中文, English, Español, Español MX, Русский)]；Español MX 空 = 同 Español。 | [(key, Chinese, English, Español, Español MX, Russian)]; empty Español MX = same as Español."""
    out = []
    with open(TABLE, encoding='utf-8') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            k = r['键']; zh, en, es, mx, ru = (r[c].replace('\\n', '\n') for c in ('中文', 'English', 'Español', 'Español MX', 'Русский'))
            if not re.fullmatch(r'dm[A-Za-z0-9]+', k) or not zh or not en or not es or not ru: raise SystemExit('dm_i18n：表里这一行不对：%r' % r)
            mx = mx or es
            for nm, t in (('英文', en), ('西语', es), ('拉美西语', mx), ('俄语', ru)):
                if k not in EN_HAS_ZH and re.search(r'[　-鿿＀-￯]', re.sub(PROPER, '', t)): raise SystemExit('dm_i18n：%s 的%s里有中文字符：%r' % (k, nm, t))
                if FMT.findall(zh) != FMT.findall(t): raise SystemExit('dm_i18n：%s 的中文与%s占位符不一致：%s / %s' % (k, nm, FMT.findall(zh), FMT.findall(t)))
                if t.count('\n') != en.count('\n'): raise SystemExit('dm_i18n：%s 的%s换行数与英文不同（行高按行数排）：%r' % (k, nm, t))
            lim = next((n for pat, n in EN_LIMITS if re.fullmatch(pat, k)), None)   # 字数闸（见 EN_LIMITS；西语 / 俄语同一栏同一上限） | Length gate (see EN_LIMITS; Spanish and Russian use the same limit for the same field)
            for nm, t in (('英文', en), ('西语', es), ('拉美西语', mx), ('俄语', ru)):
                if lim is not None and len(FMT.sub('000', t).split('\n')[0]) > lim: raise SystemExit('dm_i18n：%s 的%s「%s」%d 个字符，超过这一栏的上限 %d（会顶框 / 被截断；压短，或给这一栏做专用的小字号控件）' % (k, nm, t, len(FMT.sub('000', t).split('\n')[0]), lim))
            out.append((k, zh, en, es, mx, ru))
    ks = [r[0] for r in out]
    if len(set(ks)) != len(ks): raise SystemExit('dm_i18n：键重复：%s' % sorted({k for k in ks if ks.count(k) > 1}))
    return out

def to_traditional():
    """zhTW 列：中文按 OpenCC s2tw 逐字转繁（只换字形与台湾用字，不换词，免得和原版繁中的游戏用语对不上）。没装 opencc-python-reimplemented 就照旧放简体并提示。
    zhTW column: the Chinese converted character by character with OpenCC s2tw (glyph forms and Taiwan variants only, no phrase replacement, so terms stay in line with the original game's Traditional Chinese). Without opencc-python-reimplemented installed, Simplified is kept as before with a notice."""
    try: from opencc import OpenCC
    except ImportError:
        print('dm_i18n：没装 opencc-python-reimplemented，繁中下我们的字显示简体（pip install opencc-python-reimplemented 后重新构建）'); return lambda s: s
    return OpenCC('s2tw').convert

def build(romfs, outdir, repl):
    """repl：前面步骤已换的 {包内逻辑路径: 本地文件}。返回 ({包内逻辑路径: 本地文件}, 条数)。 | repl: {logical path in the package: local file} already replaced by earlier steps. Returns ({logical path in the package: local file}, entry count)."""
    import touch_layout   # 用时再导：touch_layout 自己在模块层要读本表（自定义页行数），顶层互导会半初始化 | imported at use time: touch_layout itself reads this table at module level (row count of the Custom page), and a top-level mutual import would leave it half-initialized
    path, text = touch_layout.read_layout(romfs, None, suffix='/lng/strings/ui.json')
    prev = repl.get(path.lower())
    if prev: text = open(prev, encoding='utf-8-sig').read()
    have = set(re.findall(r'"Key":\s*"((?:[^"\\]|\\.)*)"', text))
    rs = rows()
    clash = [r[0] for r in rs if r[0] in have]
    if clash: raise SystemExit('dm_i18n：键和原版重名：%s' % clash)
    tw = to_traditional()
    col = lambda l, zh, en, es, mx, ru: zh if l == 'zhCN' else tw(zh) if l == 'zhTW' else es if l == 'esES' else mx if l == 'esMX' else ru if l == 'ruRU' else en
    add = [{'id': ID0 + i, 'Key': k, **{l: col(l, zh, en, es, mx, ru) for l in LANGS}} for i, (k, zh, en, es, mx, ru) in enumerate(rs)]
    end = text.rstrip().rindex(']')
    body = ',\n'.join('  ' + json.dumps(e, ensure_ascii=False) for e in add)
    text = text[:end].rstrip() + ',\n' + body + '\n' + text[end:]
    json.loads(text)   # 拼完还是合法 JSON | still valid JSON after assembly
    o = os.path.join(outdir, 'i18n_ui.json'); open(o, 'w', encoding='utf-8-sig', newline='').write(text)
    return {path.lower(): o}, len(add)

def header():
    """dm/host/dm_i18n_tab.h 的内容（游戏外画的字用的编译期表）。 | Contents of dm/host/dm_i18n_tab.h (the compile-time table for text drawn outside the game)."""
    c = lambda s: json.dumps(s, ensure_ascii=False)   # JSON 串的转义在 C 里同样成立（\n \" \\） | JSON string escapes are equally valid in C (newline, double quote, backslash)
    out = ['// 生成文件，勿手改：python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h（表 dm/i18n/ui_text.tsv）', '#pragma once',
           'static const struct { const char *k, *zh, *en, *es, *mx, *ru; } k_dm_i18n[] = {']
    out += ['    { %s, %s, %s, %s, %s, %s },' % (c(k), c(zh), c(en), c(es), c(mx), c(ru)) for k, zh, en, es, mx, ru in rows()]
    return '\n'.join(out + ['};', ''])

if __name__ == '__main__':
    import sys
    if sys.argv[1:] == ['--header']: sys.stdout.write(header())
    elif sys.argv[1:] == ['--check']:   # 单测：表与入库的头文件一致 | unit test: the table matches the checked-in header file
        h = os.path.join(HERE, '..', '..', 'host', 'dm_i18n_tab.h')
        if open(h, encoding='utf-8').read() != header(): raise SystemExit('dm_i18n：dm/host/dm_i18n_tab.h 过期，重跑 python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h')
        print('dm_i18n OK %d 条' % len(rows()))
    else: raise SystemExit('用法：dm_i18n.py --header | --check')
