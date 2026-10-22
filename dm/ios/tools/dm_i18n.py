#!/usr/bin/env python3
"""我们自己加的界面文字的多语表：唯一来源 dm/i18n/ui_text.tsv（键 / 中文 / English / Español / Español MX / Русский / 한국어 / 用在哪）。
做法 = 把每一行作为新条目追加进原版字符串表 ui.json（RomFS 覆盖层，与 touch_strings 同一套机制）：zhCN / zhTW 列 = 中文（繁体玩家读简体比读英文顺），
esES 列 = Español，esMX 列 = Español MX（空 = 同 Español；只在拉美官方用词与 esES 不同时填），ruRU 列 = Русский，koKR 列 = 한국어，其余语言列 = English。
这样布局里写「@键」、宿主用原版取串 main+0x7f5630 取，语言切换由原版自己重装字符串表完成（SetLanguage，r34 起不剪），不用我们另跟语言。
'''Multilingual table for UI text we added ourselves: single source dm/i18n/ui_text.tsv (key / Chinese / English / Español / Español MX / Russian / Korean / where used).
Method = append each row as a new entry to the original string table ui.json (RomFS overlay, same mechanism as touch_strings): zhCN / zhTW columns = Chinese (Traditional Chinese players read Simplified more easily than English),
esES column = Español, esMX column = Español MX (empty = same as Español; filled only where official Latin American wording differs from esES), ruRU column = Russian, koKR column = Korean, the other language columns = English.
So layouts write "@key", the host looks up via the original main+0x7f5630, and language switching is done by the original game reloading the string table (SetLanguage, not cut since r34); we don't track language separately.'''"""
import csv, json, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, '..', '..', 'i18n', 'ui_text.tsv')
LANGS = ('enUS', 'zhTW', 'deDE', 'esES', 'frFR', 'itIT', 'koKR', 'plPL', 'esMX', 'jaJP', 'ptBR', 'ruRU', 'zhCN')   # 原版条目的列序 | column order of the original entries
ID0 = 300000   # 原版全部字符串表最大 id 251780；我们的条目从这里起按表序编号 | the largest id across all original string tables is 251780; our entries are numbered from here in table order

PROPER = '老机型补充包|D2R Mobile Resources|D2R Mobile'   # 英文里唯一许可的中文：App 显示名与资源文件夹名
FMT = re.compile(r'%[-+ #0-9.]*(?:ll|l|h|z)?[a-zA-Z@%]')

EN_LIMITS = (
    (r'dmRiftSay(?!Prefix).*', 31),
    (r'dmRiftPtsGot', 62), (r'dmRiftPtsHeld', 33),
    (r'dmRift(Range|Best|Str|Lvl|Time)', 13),
    (r'dmRiftGam(Uni|Set)', 8), (r'dmRiftGamAfterEn', 17),
    (r'dmRift(SelT|InfT|GamT|TitleRun|TitleDone|TitleLate)', 16),
    (r'dmRift(GuardDown|NextOpen|Saved|RetryOpen|NotCounted)', 16),
    (r'dmRiftCap.*', 20),
    (r'dmRiftBtn.*', 12),
    (r'dmRiftMsItem\d+', 15), (r'dmRiftMsSub\d+', 19), (r'dmRiftMsTier\d+', 4), (r'dmRiftMs(Got|Next|No)', 7), (r'dmRiftMsT', 12), (r'dmRiftMsNEn', 38),
    (r'dmRiftHud(Title)', 4), (r'dmRiftHud(Done|Late|Guard)', 18), (r'dmRiftHud(Next|Same|Kill|Goal)', 26),
    (r'dmKbRiftUnlock', 9), (r'dmKbRift(Best|Total)', 16),
)
# Keys that must show Chinese characters in the English UI as well (language row of Custom page)
EN_HAS_ZH = {'dmCustomLangZh', 'dmCustomLangHintZh'}
# CJK check: skip for Korean column (Hangul is expected)
CJK = re.compile(r'[　-鿿＀-￯]')

def rows():
    """[(键, 中文, English, Español, Español MX, Русский, 한국어)]; empty Español MX = same as Español."""
    out = []
    with open(TABLE, encoding='utf-8') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            k = r['键']
            zh, en, es, mx, ru, ko = (r[c].replace('\\n', '\n') for c in
                ('中文', 'English', 'Español', 'Español MX', 'Русский', '한국어'))
            if not re.fullmatch(r'dm[A-Za-z0-9]+', k) or not zh or not en or not es or not ru or not ko:
                raise SystemExit('dm_i18n：表里这一行不对：%r' % r)
            mx = mx or es
            for nm, t in (('英文', en), ('西语', es), ('拉美西语', mx), ('俄语', ru)):
                if k not in EN_HAS_ZH and CJK.search(re.sub(PROPER, '', t)):
                    raise SystemExit('dm_i18n：%s 的%s里有中文字符：%r' % (k, nm, t))
                if FMT.findall(zh) != FMT.findall(t):
                    raise SystemExit('dm_i18n：%s 的中文与%s占位符不一致：%s / %s' % (k, nm, FMT.findall(zh), FMT.findall(t)))
                if t.count('\n') != en.count('\n'):
                    raise SystemExit('dm_i18n：%s 的%s换行数与英文不同（行高按行数排）：%r' % (k, nm, t))
            # Korean: placeholders and newlines must match Chinese / English rules
            if FMT.findall(zh) != FMT.findall(ko):
                raise SystemExit('dm_i18n：%s 的中文与韩语占位符不一致：%s / %s' % (k, FMT.findall(zh), FMT.findall(ko)))
            if ko.count('\n') != en.count('\n'):
                raise SystemExit('dm_i18n：%s 的韩语换行数与英文不同（行高按行数排）：%r' % (k, ko))
            lim = next((n for pat, n in EN_LIMITS if re.fullmatch(pat, k)), None)
            for nm, t in (('英文', en), ('西语', es), ('拉美西语', mx), ('俄语', ru), ('韩语', ko)):
                if lim is not None and len(FMT.sub('000', t).split('\n')[0]) > lim:
                    raise SystemExit('dm_i18n：%s 的%s「%s」%d 个字符，超过这一栏的上限 %d（会顶框 / 被截断；压短，或给这一栏做专用的小字号控件）' % (k, nm, t, len(FMT.sub('000', t).split('\n')[0]), lim))
            out.append((k, zh, en, es, mx, ru, ko))
    ks = [r[0] for r in out]
    if len(set(ks)) != len(ks):
        raise SystemExit('dm_i18n：键重复：%s' % sorted({k for k in ks if ks.count(k) > 1}))
    return out

def build(romfs, outdir, repl):
    """repl：前面步骤已换的 {包内逻辑路径: 本地文件}。返回 ({包内逻辑路径: 本地文件}, 条数)。"""
    import touch_layout
    path, text = touch_layout.read_layout(romfs, None, suffix='/lng/strings/ui.json')
    prev = repl.get(path.lower())
    if prev:
        text = open(prev, encoding='utf-8-sig').read()
    have = set(re.findall(r'"Key":\s*"((?:[^"\\]|\\.)*)"', text))
    rs = rows()
    clash = [r[0] for r in rs if r[0] in have]
    if clash:
        raise SystemExit('dm_i18n：键和原版重名：%s' % clash)
    def col(l, zh, en, es, mx, ru, ko):
        if l in ('zhCN', 'zhTW'):
            return zh
        if l == 'esES':
            return es
        if l == 'esMX':
            return mx
        if l == 'ruRU':
            return ru
        if l == 'koKR':
            return ko
        return en
    add = [{'id': ID0 + i, 'Key': k, **{l: col(l, zh, en, es, mx, ru, ko) for l in LANGS}}
           for i, (k, zh, en, es, mx, ru, ko) in enumerate(rs)]
    end = text.rstrip().rindex(']')
    body = ',\n'.join('  ' + json.dumps(e, ensure_ascii=False) for e in add)
    text = text[:end].rstrip() + ',\n' + body + '\n' + text[end:]
    json.loads(text)
    o = os.path.join(outdir, 'i18n_ui.json')
    open(o, 'w', encoding='utf-8-sig', newline='').write(text)
    return {path.lower(): o}, len(add)

def header():
    """dm/host/dm_i18n_tab.h 的内容（游戏外画的字用的编译期表）。"""
    c = lambda s: json.dumps(s, ensure_ascii=False)
    out = ['// 生成文件，勿手改：python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h（表 dm/i18n/ui_text.tsv）',
           '#pragma once',
           'static const struct { const char *k, *zh, *en, *es, *mx, *ru, *ko; } k_dm_i18n[] = {']
    out += ['    { %s, %s, %s, %s, %s, %s, %s },' % (c(k), c(zh), c(en), c(es), c(mx), c(ru), c(ko))
            for k, zh, en, es, mx, ru, ko in rows()]
    return '\n'.join(out + ['};', ''])

if __name__ == '__main__':
    import sys
    if sys.argv[1:] == ['--header']:
        sys.stdout.write(header())
    elif sys.argv[1:] == ['--check']:
        h = os.path.join(HERE, '..', '..', 'host', 'dm_i18n_tab.h')
        # When run outside the repo tree for testing, only validate the table
        if os.path.isfile(h):
            if open(h, encoding='utf-8').read() != header():
                raise SystemExit('dm_i18n：dm/host/dm_i18n_tab.h 过期，重跑 python3 dm/ios/tools/dm_i18n.py --header > dm/host/dm_i18n_tab.h')
        print('dm_i18n OK %d 条' % len(rows()))
    else:
        raise SystemExit('用法：dm_i18n.py --header | --check')
