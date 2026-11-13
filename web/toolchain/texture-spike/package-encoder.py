#!/usr/bin/env python3
"""仅供构建：交付精确原始 bc3 函数，不交付文件系统/ctypes 入口。
Build-only: ship exact original bc3 function, never its filesystem/ctypes entrypoints."""

import ast
import hashlib
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = ROOT / "dm/ios/tools/gen_widestash_art.py"
PIN = "f58435e650cc12e1472eb229a7eb5d4e08f7477f37f5d91376df5ad5af952c79"
data = SOURCE.read_bytes()
if hashlib.sha256(data).hexdigest() != PIN:
    raise SystemExit("gen_widestash_art.py source drift; review and revalidate encoder")
module = ast.parse(data)
node = next(n for n in module.body if isinstance(n, ast.FunctionDef) and n.name == "bc3")
source = ast.get_source_segment(data.decode(), node)
# 上游只改了本机库加载；编码函数必须保持逐字节不变。
# Upstream changed only native library loading; the encoder function stays byte-identical.
if (
    hashlib.sha256(source.encode()).hexdigest()
    != "97ac9d6f684ecbcdbf4ba76481a7cc71624333c1e4099f0ba1601f677ea3b0c2"
):
    raise SystemExit("BC3 function changed; review Rust encoder parity")
output = Path(os.environ.get("TEXTURE_OUTPUT_DIR", HERE / "dist"))
output.mkdir(exist_ok=True)
(output / "original_bc3.py").write_text(
    "import struct\nimport numpy as np\nfrom PIL import Image\n\n" + source + "\n"
)
(output / "encoder-provenance.json").write_text(
    json.dumps(
        {
            "source": str(SOURCE.relative_to(ROOT)),
            "sha256": PIN,
            "function": "bc3",
            "method": "Unmodified AST source segment; bounded independent four-row bands",
        },
        indent=2,
    )
    + "\n"
)
