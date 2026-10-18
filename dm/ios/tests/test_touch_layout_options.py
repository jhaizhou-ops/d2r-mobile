"""现有自定义页中两位置换组开关的跨语言契约。 | Cross-language contract for the two-position switch in the existing Custom page. | Cross-language contract for the two-position switch in the existing Custom page."""
import json
from pathlib import Path
import re
import sys

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
for name in ('TouchCustom_touch_swap_above', 'TouchCustom_touch_swap_above_near'):
    assert nodes[name]['type'] == 'ToggleButtonWidget'
    assert nodes[name]['fields']['onClickMessage'].endswith(name)
assert nodes['TouchCustom_touch_swap_above_Text']['fields']['rect']['height'] == 360
for suffix in ('Near', 'Far'):
    assert any(n.get('fields', {}).get('text') == '@dmCustomTouchSwap' + suffix for n in nodes.values())
host = (root / 'host/dm_custom.c').read_text()
keys = re.findall(r'"([a-z_0-9]+)"', re.search(r'k_keys\[\].*?=\s*\{(.*?)\}', host, re.S)[1])
assert keys == [key for key, _ in layout._CUSTOM_ALL]
defaults = [int(x) for x in re.search(r'k_def\[NK\].*?=\s*\{(.*?)\}', host, re.S)[1].split(',')]
assert len(defaults) == len(keys) and defaults[keys.index('touch_swap_above')] == 0
assert 'TouchCustom_touch_swap_above_near' in host
print('PASS touch layout options: generated radio pair, labels, defaults, host order and scrolling row extent')
