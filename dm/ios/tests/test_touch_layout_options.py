"""现有自定义页中两位置换组开关的跨语言契约。 | Cross-language contract for the two-position switch in the existing Custom page. | Cross-language contract for the two-position switch in the existing Custom page."""
import importlib
import json
import os
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'ios/tools'))
os.environ.pop('MOVIE_FFMPEG', None)   # 缺省包的自定义页（「配音」行只在 MOVIE_FFMPEG=1 时有，下面另核）
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
# 暗处加亮（bright，#149）：三项单选，三个勾选框各发自己的点击消息，整行三行高，三项标题都在
# Brighten dark places (bright, #149): a three-option radio, each of the three checkboxes sends its own click message, the row is three rows high, all three titles are present
for name in ('TouchCustom_bright', 'TouchCustom_bright_mid', 'TouchCustom_bright_full'):
    assert nodes[name]['type'] == 'ToggleButtonWidget'
    assert nodes[name]['fields']['onClickMessage'].endswith(name)
assert nodes['TouchCustom_bright_Text']['fields']['rect']['height'] == 540
for suffix in ('Off', 'Mid', 'Full'):
    assert any(n.get('fields', {}).get('text') == '@dmCustomBright' + suffix for n in nodes.values())
# 3D 清晰度（scale3d，#149）：三项单选，同暗处加亮
# 3D resolution (scale3d, #149): a three-option radio, like brightening
for name in ('TouchCustom_scale3d', 'TouchCustom_scale3d_mid', 'TouchCustom_scale3d_full'):
    assert nodes[name]['type'] == 'ToggleButtonWidget'
    assert nodes[name]['fields']['onClickMessage'].endswith(name)
assert nodes['TouchCustom_scale3d_Text']['fields']['rect']['height'] == 540
for suffix in ('Low', 'Mid', 'Full'):
    assert any(n.get('fields', {}).get('text') == '@dmCustomScale3d' + suffix for n in nodes.values())
host_src = (root / 'host/dm_custom.c').read_text()
def host_variant(voice):   # 宿主按 VOICE_ROW（DM_MOVIE_FFMPEG）条件编译的两份键表：缺省去掉 #if VOICE_ROW 块，开关包只去掉预处理行
    keep = re.sub(r'#if VOICE_ROW\n(.*?)#endif\n', (lambda m: m[1]) if voice else '', host_src, flags=re.S)
    return re.sub(r'(?m)^#.*\n', '', keep)
for voice in (False, True):
    if voice:
        os.environ['MOVIE_FFMPEG'] = '1'; layout = importlib.reload(layout)
    host = host_variant(voice)
    keys = re.findall(r'"([a-z_0-9]+)"', re.search(r'k_keys\[\].*?=\s*\{(.*?)\}', host, re.S)[1])
    assert keys == [key for key, _ in layout._CUSTOM_ALL], (voice, keys)
    assert ('voice' in keys) == voice
    defaults = [int(x) for x in re.search(r'k_def\[NK\].*?=\s*\{(.*?)\}', host, re.S)[1].split(',')]
    assert len(defaults) == len(keys) and defaults[keys.index('touch_swap_above')] == 0
    assert defaults[keys.index('bright')] == 2
    assert defaults[keys.index('scale3d')] == 1
os.environ.pop('MOVIE_FFMPEG'); layout = importlib.reload(layout)
# 安卓导入包（DM_OVERLAY_NO_ICLOUD=1）：没有 iCloud 那一行，下一行顶到它原来的位置；键表照旧含 icloud（与宿主同序）
# Android import pack (DM_OVERLAY_NO_ICLOUD=1): no iCloud row, the next row moves up to its place; the key list still contains icloud (same order as the host)
all_keys = [k for k, _ in layout._CUSTOM_ALL]; y_icloud = layout._CUSTOM_Y[all_keys.index('icloud')]
os.environ['DM_OVERLAY_NO_ICLOUD'] = '1'; layout = importlib.reload(layout)
out, _ = layout._custom_page('"name": "VideoOptionsPanel", "name": "OptionsTable"')
assert 'TouchCustom_icloud' not in out and 'dmCustomIcloud' not in out
assert re.search(r'"name": "TouchCustom_pot_pick_Text", "fields": \{ "rect": \{ "x": 0, "y": %d,' % y_icloud, out)
assert [k for k, _ in layout._CUSTOM_ALL] == all_keys
assert 'TouchCustom_scale3d_full' in out
os.environ.pop('DM_OVERLAY_NO_ICLOUD'); layout = importlib.reload(layout)
assert 'TouchCustom_icloud' in layout._custom_page('"name": "VideoOptionsPanel", "name": "OptionsTable"')[0]
# Mac App（DM_OVERLAY_MAC）：没有 3D 清晰度那一行，iCloud 行还在；键表不变
# Mac App (DM_OVERLAY_MAC): no 3D resolution row, the iCloud row is still there; the key list is unchanged
os.environ['DM_OVERLAY_MAC'] = '1'; layout = importlib.reload(layout)
out = layout._custom_page('"name": "VideoOptionsPanel", "name": "OptionsTable"')[0]
assert 'TouchCustom_scale3d' not in out and 'dmCustomScale3d' not in out and 'TouchCustom_icloud' in out
assert [k for k, _ in layout._CUSTOM_ALL] == all_keys
os.environ.pop('DM_OVERLAY_MAC'); layout = importlib.reload(layout)
host = host_src
assert 'TouchCustom_touch_swap_above_near' in host
print('PASS touch layout options: generated radio pair, brightness radio, labels, defaults, host order (default and MOVIE_FFMPEG=1), no iCloud row for Android, 3D resolution radio (not on the Mac App) and scrolling row extent')
