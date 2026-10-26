from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import wide_stash

vault = wide_stash.vault_widgets()
top = next(node for node in vault['children'] if node['name'] == 'TouchV_Top')
tabs = top['children'][:2]
for tab, label in zip(tabs, ('@Unique', '@Sets'), strict=True):
    rect = tab['fields']['rect']
    text = tab['children'][0]['fields']
    # The sprite frame is 179 x 47 pixels, i.e. 358 x 94 game UI units.
    # 精灵帧 179 x 47 像素，对应 358 x 94 游戏界面单位。
    assert rect['width'] == 358 and rect['height'] == 94, rect
    assert text['rect']['x'] + text['rect']['width'] <= rect['width']
    assert text['rect']['y'] + text['rect']['height'] <= rect['height']
    assert text['text'] == label
    assert tab['fields']['onClickMessage'].endswith(tab['name'])
    assert rect['y'] + rect['height'] * rect['scale'] <= top['fields']['rect']['height']
left, right = (tab['fields']['rect'] for tab in tabs)
assert left['x'] + left['width'] * left['scale'] < right['x']
search = next(node for node in top['children'] if node['name'] == 'TouchV_SearchBg')
assert right['x'] + right['width'] * right['scale'] < search['fields']['rect']['x']
print('PASS vault tab art/label bounds, row height, separation, messages')
