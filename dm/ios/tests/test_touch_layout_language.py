"""自定义页语言那一行：四项单选「中文 / English / Español / Русский」的跨语言契约。
语言行的控件名、勾选框列位置、四个选项各自的正文字宽与宿主 dm_custom.c 的表必须一致——这一行挤不下第五项，
所以位置是硬编码的，改一行字就要回来量这里。
'''Language row of the Custom page: contract for the four-option radio "中文 / English / Español / Русский".
The widget names, the checkbox column, the per-option text width and the host's dm_custom.c table must agree --
this row has no room for a fifth option, so the geometry is hard-coded and any wording change has to re-check it here.'''"""
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

opts = (('zh', 'Zh', 'dmCustomLangZh'), ('en', 'En', 'dmCustomLangEnOpt'), ('es', 'Es', 'dmCustomLangEsOpt'), ('ru', 'Ru', 'dmCustomLangRuOpt'))
for key, cap, label in opts:
    box = nodes['TouchCustom_lang_' + key]
    assert box['type'] == 'ToggleButtonWidget', key
    assert box['fields']['onClickMessage'].endswith('TouchCustom_lang_' + key), key
    assert nodes['TouchCustomLang%s_Text' % cap]['fields']['text'] == '@' + label, key
    assert nodes['TouchCustomLangHint%s' % cap]['fields']['text'] == '@dmCustomLangHint' + cap, key
# 最右那一格（Русский）留在各行勾选框那一列：别的行也用它
# The rightmost cell (Русский) stays in the checkbox column of the other rows: the other rows use it too
assert nodes['TouchCustom_lang_ru']['fields']['rect']['x'] == 1315
assert nodes['TouchCustomLangHintRu']['fields']['rect']['x'] > 10000, 'hint starts off screen and the host moves it'
# 四个选项按顺序从左到右，文字框有正宽度且互不重叠
# The four options run left to right in order; text boxes have positive width and do not overlap
x = []
for key, cap, _ in opts:
    t = nodes['TouchCustomLang%s_Text' % cap]['fields']['rect']
    b = nodes['TouchCustom_lang_' + key]['fields']['rect']
    assert t['width'] >= 195, (key, t)   # 180 宽时 English / Español / Русский 折成「Englis h」（真机截图） | At 180 wide English / Español / Русский wrap to "Englis h" (device screenshot)
    assert t['x'] + t['width'] <= b['x'], (key, t, b)
    x.append(t['x'])
assert x == sorted(x) and len(set(x)) == 4, x
host = (root / 'host/dm_custom.c').read_text()
boxes = re.findall(r'"(TouchCustom_lang_[a-z]+)"', re.search(r'k_lang_box\[LANG_N\].*?=\s*\{(.*?)\}', host, re.S)[1])
assert boxes == ['TouchCustom_lang_' + k for k, _, _ in opts], boxes
assert re.search(r'#define LANG_N 4\b', host), 'LANG_N must be 4'
print('PASS touch layout language row: four-option radio, labels, hints, checkbox column and host order')
