#!/usr/bin/env python3
# 仅提取原生纯解码函数，验证合成输入，不导入游戏工具或读取游戏文件。
# Extract only native pure decode functions for synthetic input, without importing game tools or reading game files.
import ast
import base64
import json
from pathlib import Path
import sys

import numpy as np

source = Path(__file__).resolve().parents[3] / "dm/tools/astc_pack.py"
names = {"bc3_decode", "blocks_to_image"}
tree = ast.parse(source.read_text())
functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
if len(functions) != len(names):
    raise RuntimeError("Native pure reference functions missing")
namespace = {"np": np, "ROW_BLOCKS": 1024}
exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), "exec"), namespace)
request = json.loads(sys.stdin.read(2_000_000))
raw = base64.b64decode(request["bc3"], validate=True)
if not raw or len(raw) % 16 or len(raw) > 65536 * 16:
    raise ValueError("Synthetic reference input exceeds primitive bounds")
px = namespace["bc3_decode"](raw)
grid = namespace["blocks_to_image"](px)
print(
    json.dumps(
        {
            "rgba": base64.b64encode(px.tobytes()).decode(),
            "grid": base64.b64encode(grid.tobytes()).decode(),
            "width": grid.shape[1],
            "height": grid.shape[0],
        }
    )
)
