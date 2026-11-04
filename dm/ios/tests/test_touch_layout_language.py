"""自定义页语言那一行：六项单选「中文 / English / Español / Русский / 한국어 / Português」的跨语言契约。
六项一排放不下，排成两行三列。语言行的控件名、勾选框列位置、每项的正文字宽与宿主 dm_custom.c 的表必须一致，
所以位置是硬编码的，改一行字就要回来量这里。"""
import json, re, sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'ios/tools'))
import touch_layout as layout

out, _ = layout._custom_page('"name": "VideoOptionsPanel", "name": "OptionsTable"')
page = json.loads(re.sub(r',\s*([}\]])', r'\1', out))
nodes = {}
def walk(node):
    if isinstance(node, dict):
        if 'name' in node:
            assert node['name'] not in nodes, node['name']
            nodes[node['name']] = node
        for child in node.values():
            walk(child)
    elif isinstance(node, list):
        for child in node:
            walk(child)
walk(page)

# （勾选框后缀，控件名后缀，文字键，行号，最小文字框宽）
opts = (('zh', 'Zh', 'dmCustomLangZh', 0, 131), ('en', 'En', 'dmCustomLangEnOpt', 0, 196), ('es', 'Es', 'dmCustomLangEsOpt', 0, 213),
        ('ru', 'Ru', 'dmCustomLangRuOpt', 1, 221), ('ko', 'Ko', 'dmCustomLangKoOpt', 1, 166), ('pt', 'Pt', 'dmCustomLangPtOpt', 1, 290))
for key, cap, label, _, _ in opts:
    box = nodes['TouchCustom_lang_' + key]
    assert box['type'] == 'ToggleButtonWidget', key
    assert box['fields']['onClickMessage'].endswith('TouchCustom_lang_' + key), key
    assert nodes['TouchCustomLang%s_Text' % cap]['fields']['text'] == '@' + label, key
    assert nodes['TouchCustomLangHint%s' % cap]['fields']['text'] == '@dmCustomLangHint' + cap, key
    assert nodes['TouchCustomLangHint%s' % cap]['fields']['rect']['x'] > 10000, 'hint starts off screen and the host moves it'
# 每项勾选框在前、字在后（字在勾选框右边 10）；同一行从左到右不重叠，全在各行勾选框那一列的右沿（1385）以内
# 文字框宽：实宽按俄语界面真包截图量（中文 99、English 166、Español 178、Русский 188；한국어按字体同比约 136，
# Português 按重音字母同比约 260），框比实宽多 30 不折行（180 宽时 English 折成「Englis h」）
for row in (0, 1):
    right = 0
    for key, cap, _, r, minw in (o for o in opts if o[3] == row):
        t = nodes['TouchCustomLang%s_Text' % cap]['fields']['rect']
        b = nodes['TouchCustom_lang_' + key]['fields']['rect']
        assert t['width'] >= minw, (key, t)
        assert b['x'] >= right, (key, b)
        assert t['x'] == b['x'] + 80, (key, b, t)
        assert t['y'] == b['y'], (key, b, t)
        right = t['x'] + t['width']
    assert right <= 1385, (row, right)
# 两行：同一列的第二行在第一行下面一整行（70），行高够放下两行与提示
r0 = nodes['TouchCustom_lang_ru']['fields']['rect']
r1 = nodes['TouchCustom_lang_zh']['fields']['rect']
assert r0['y'] == r1['y'] + 70, (r0, r1)
lab = nodes['TouchCustom_lang_Text']['fields']
assert lab['text'] == '' and lab['rect']['x'] == 0 and lab['rect']['width'] >= 1385, lab
assert lab['rect']['height'] >= r0['y'] - lab['rect']['y'] + 70, lab
host = (root / 'host/dm_custom.c').read_text()
boxes = re.findall(r'"(TouchCustom_lang_[a-z]+)"', re.search(r'k_lang_box\[LANG_N\].*?=\s*\{(.*?)\}', host, re.S)[1])
assert boxes == ['TouchCustom_lang_' + k for k, _, _, _, _ in opts], boxes
hints = re.findall(r'"(TouchCustomLangHint[A-Za-z]+)"', re.search(r'k_lang_hint\[LANG_N\].*?=\s*\{(.*?)\}', host, re.S)[1])
assert hints == ['TouchCustomLangHint' + cap for _, cap, _, _, _ in opts], hints
assert re.search(r'#define LANG_N 6\b', host), 'LANG_N must be 6'
print('PASS touch layout language row: six-option radio in two rows of three, labels, hints, checkbox column and host order')
