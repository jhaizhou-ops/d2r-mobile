"""通过 AST 只加载纯反射定义，绝不运行原生构建顶层代码。刻意使用未修改原生实现，而非 JS 移植。
Load only pure reflection definitions by AST, never run native build top-level.
This deliberately uses the unchanged native implementation, not the JS port.
"""

import ast
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[3]
source = (root / "dm/gfx/tools/dmsh_build.py").read_text()
names = {"raster_facts", "decode_tex", "reflect", "drv_reads", "unfillable"}
assigns = {"TEX_BASE", "SAMP_BASE", "IMG_BASE", "DRV_FILL", "XC", "RULES"}
tree = ast.parse(source)
nodes = [
    n
    for n in tree.body
    if isinstance(n, ast.FunctionDef)
    and n.name in names
    or isinstance(n, ast.Assign)
    and any(isinstance(x, ast.Name) and x.id in assigns for t in n.targets for x in ast.walk(t))
]
ns = {"re": re}
exec(
    compile(ast.Module(body=nodes, type_ignores=[]), "<native-pure-reflection>", "exec"),
    ns,
)
assert ns["RULES"] == [], "Native MSL rewrite rules changed; port before proceeding"
assert ns["XC"] == [
    "--msl",
    "--msl-version",
    "20300",
    "--msl-argument-buffers",
    "--msl-texture-buffer-native",
    "--msl-pad-fragment-output",
]


def fields(stage, src, resources):
    entry = re.search(r"(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(", src)
    reads = ns["drv_reads"](src)
    return dict(
        entry=entry.group(2) if entry else "main0",
        msl_entry_kind=entry.group(1) if entry else None,
        drv_reads=reads,
        drv_unfillable=ns["unfillable"](reads),
        bindings=ns["reflect"]("synthetic", stage, src, resources),
        vertex_inputs=[
            dict(location=int(m.group(2)), msl_type=m.group(1))
            for m in re.finditer(r"(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]", src)
        ],
        raster=ns["raster_facts"](stage, src),
    )


if __name__ == "__main__":
    payload = json.load(sys.stdin)
    json.dump(
        [fields(x["stage"], x["msl"], x["resources"]) for x in payload],
        sys.stdout,
        ensure_ascii=False,
    )
