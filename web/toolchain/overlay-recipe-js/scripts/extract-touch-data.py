"""仅供构建的可信静态触控控件，不含游戏数据或运行时 Python。
Build-only trusted static touch widgets; no game data or runtime Python."""

import argparse
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
OUT = Path(__file__).resolve().parents[1] / "touch-data.json"
PINS = {
    "dm/ios/tools/touch_layout.py": "600e17649f2e3163add19b8d7b15b52b23e658f5e668e9bbb6c480cbde1fca86",
    "dm/host/dm_kills.c": "8a0de7a29ce473621dc94eee60fc397f14058309eb1e96ba394a54835d1bb436",
}
for name, expected in PINS.items():
    if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
        raise SystemExit("Touch static source drift: " + name)
os.environ.pop("DM_OVERLAY_MAC", None)
os.environ.pop("DM_CUSTOM_TEST_ROWS", None)
os.environ["MOVIE_FFMPEG"] = "0"
source = ROOT / "dm/ios/tools/touch_layout.py"
spec = importlib.util.spec_from_file_location("pinned_touch_static", source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
tree = ast.parse(source.read_text())


def string(function, contains):
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == function)
    matches = [
        n.value
        for n in ast.walk(node)
        if isinstance(n, ast.Constant) and isinstance(n.value, str) and contains in n.value
    ]
    assert len(matches) == 1, (function, contains, len(matches))
    return matches[0]


module._rw_children_check()
data = {
    "sourceSha256": PINS,
    "environment": {
        "DM_OVERLAY_MAC": False,
        "DM_CUSTOM_TEST_ROWS": 0,
        "MOVIE_FFMPEG": False,
    },
}
for name in (
    "OFF",
    "FRONTEND",
    "INSERT",
    "CLASS_STRIPS",
    "PCFRONT_PLAIN",
    "INGAME",
    "PAD_VENDOR",
    "BIG_FONT",
    "_UPDATE",
    "_UPDATE_PAD",
    "_UPDATE_STUB",
    "_RW",
):
    data[name] = getattr(module, name)
data["customPage"] = module._custom_page('"name": "VideoOptionsPanel" "name": "OptionsTable"')
data["riftPanel"] = module._rift_panel(
    '"type": "MessageLogPanel", "name": "MessageLogPanel" "name": "ListContainer" "name": "ScrollBar" "name": "ChatEntryTemplate"'
)
data["killboardJson"] = json.dumps(module._kb_board(), ensure_ascii=False)
data["chronicleClose"] = string("_chronicle_touch", "TouchCloseBox")
data["padDiffText"] = string("pad_pause", "TouchPadDiffTxt") % 278
data["blissKey"] = string("pad_bliss", '"name": "TouchBlissKey"')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--check",
    action="store_true",
    help="Verify existing data, ignoring formatter whitespace only",
)
args = parser.parse_args()
if args.check:
    existing = json.loads(OUT.read_text())
    # 保留字典插入顺序及精确内嵌控件文本。
    # Preserve insertion order in dictionaries as well as exact embedded widget text.
    if json.dumps(existing, ensure_ascii=False) != json.dumps(data, ensure_ascii=False):
        raise SystemExit("Touch static widget data drift")
else:
    OUT.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
print(
    json.dumps(
        {
            "file": str(OUT.relative_to(ROOT)),
            "bytes": OUT.stat().st_size,
            "sourcePinned": True,
            "checked": args.check,
        }
    )
)
