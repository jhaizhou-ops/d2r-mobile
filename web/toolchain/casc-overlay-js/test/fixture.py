"""构造源自原生编码器的合成 TVFS 请求。 | Build synthetic TVFS requests with original encoders."""

import ast
import base64
import importlib.util
import json
from pathlib import Path
import struct
import hashlib

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "dm/tools/casc_overlay.py"
spec = importlib.util.spec_from_file_location("native", SOURCE)
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)
source = ROOT / "web/toolchain/asset-generation-spike/test/casc-overlay-oracle.py"
nodes = [
    n
    for n in ast.parse(source.read_text()).body
    if isinstance(n, ast.FunctionDef) and n.name in {"tvfs", "fixture"}
]
namespace = {"native": native, "struct": struct, "hashlib": hashlib}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), "exec"), namespace)
files, vfs, root, keys = namespace["fixture"]()
replacements = [
    ("data/n.bin", b"", False),
    ("data/z.bin", b"zlib replacement" * 100, False),
    ("data/b.bin", bytes(range(256)) * 1100, False),
    ("data/x/new.bin", b"added original trie", True),
]
parts = [files["data/.build.config"], vfs, root] + [r[1] for r in replacements]
jobs = [
    {"path": path, "add": add, **({"oldEkey": keys[i][0].hex()} if not add else {})}
    for i, (path, _, add) in enumerate(replacements)
]
print(
    json.dumps(
        {
            "payload": base64.b64encode(b"".join(parts)).decode(),
            "options": {
                "parts": list(map(len, parts)),
                "jobs": jobs,
                "location": [0, 2],
            },
        }
    )
)
