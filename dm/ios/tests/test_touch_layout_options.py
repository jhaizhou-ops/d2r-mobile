"""现有自定义页中两位置换组开关的跨语言契约。 | Cross-language contract for the two-position switch in the existing Custom page. | Cross-language contract for the two-position switch in the existing Custom page."""
import json
import os
from pathlib import Path
import re
import sys

# q_res（分辨率档）是 Android 专属行：宿主键表里有、iOS 出包不带。按 Android 出包口径（DM_TOUCH_RES_TIER=1）比，
# 同时把 C 文件里的 #if/#endif 去掉，这样键表与布局都是含 q_res 的那一份。
# q_res (resolution tier) is an Android-only row: present in the host key table, absent from the iOS package. Compare with the
# Android packaging convention (DM_TOUCH_RES_TIER=1) and strip #if/#endif from the C file so both sides are the variant with q_res.
os.environ.setdefault('DM_TOUCH_RES_TIER', '1')
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
host = re.sub(r'(?m)^\s*#.*$', '', host)   # 去掉 #if/#endif：按 Android 那份比（q_res 在 __ANDROID__ 里） | strip #if/#endif: compare the Android variant (q_res lives under __ANDROID__)
host = re.sub(r'//[^\n]*', '', host)       # 去掉行尾注释，键表/缺省表里有中文说明 | strip end-of-line comments; the key/default tables carry Chinese notes
keys = re.findall(r'"([a-z_0-9]+)"', re.search(r'k_keys\[\].*?=\s*\{(.*?)\}', host, re.S)[1])
assert keys == [key for key, _ in layout._CUSTOM_ALL]
defaults = [int(x) for x in re.search(r'k_def\[NK\].*?=\s*\{(.*?)\}', host, re.S)[1].split(',')]
assert len(defaults) == len(keys) and defaults[keys.index('touch_swap_above')] == 0
assert 'TouchCustom_touch_swap_above_near' in host
print('PASS touch layout options: generated radio pair, labels, defaults, host order and scrolling row extent')
