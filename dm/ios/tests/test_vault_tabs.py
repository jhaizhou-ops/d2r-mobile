# Check Chronicle tab bounds and ordinary image layering.
# 检查编年史页签尺寸及普通图片绘制层级。
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import wide_stash

vault = wide_stash.vault_widgets()
top = next(node for node in vault['children'] if node['name'] == 'TouchV_Top')
tabs = top['children'][:2]
for tab, label in zip(tabs, ('@Unique', '@Sets'), strict=True):
    rect = tab['fields']['rect']
    children = {node['name']: node for node in tab['children']}
    assert tab['type'] == 'Widget', tab['type']
    assert children['Img']['type'] == 'ImageWidget'
    assert children['Img']['fields']['frame'] == 0
    text = children['Text']['fields']
    # The sprite frame is 179 x 47 pixels, i.e. 358 x 94 game UI units.
    # 精灵帧 179 x 47 像素，对应 358 x 94 游戏界面单位。
    assert rect['width'] == 358 and rect['height'] == 94, rect
    assert text['rect']['x'] + text['rect']['width'] <= rect['width']
    assert text['rect']['y'] + text['rect']['height'] <= rect['height']
    assert text['text'] == label
    assert children['Hit']['fields']['rect']['width'] == rect['width']
    assert children['Hit']['fields']['rect']['height'] == rect['height']
    assert rect['y'] + rect['height'] * rect['scale'] <= top['fields']['rect']['height']
left, right = (tab['fields']['rect'] for tab in tabs)
assert left['x'] + left['width'] * left['scale'] < right['x']
search = next(node for node in top['children'] if node['name'] == 'TouchV_SearchBg')
assert right['x'] + right['width'] * right['scale'] < search['fields']['rect']['x']
assert vault['children'].index(top) < next(i for i, node in enumerate(vault['children']) if node['name'] == 'TouchV_TipBg')
print('PASS vault tab art/label bounds, hit regions, separation, image layering')
