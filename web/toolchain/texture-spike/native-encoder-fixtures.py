#!/usr/bin/env python3
"""独立整图原始编解码器加原始原生打包参考，仅用合成内容。
Independent full-image original codec + original native pack oracle; synthetic only."""

import ast
import base64
import ctypes as C
import json
import importlib
from pathlib import Path
import runpy
import struct
import sys
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
runpy.run_path(str(HERE / "package-encoder.py"))
# 同时检查现有解码器语料并构建未修改的原生库。
# This also checks the existing decoder corpus and builds the unchanged native library.
native = runpy.run_path(str(HERE / "native-fixtures.py"))
lib, Desc, Layout = native["lib"], native["Desc"], native["Layout"]
source = (HERE.parents[2] / "dm/ios/tools/gen_widestash_art.py").read_text()
tree = ast.parse(source)
original = {
    "C": C,
    "np": np,
    "Image": Image,
    "struct": struct,
    "Desc": Desc,
    "Layout": Layout,
}
for name in ("bc3", "desc", "pack"):
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == name)
    exec(
        compile(
            ast.Module(body=[node], type_ignores=[]),
            str(HERE.parents[2] / "dm/ios/tools/gen_widestash_art.py"),
            "exec",
        ),
        original,
    )
sys.path[:0] = [str(HERE / "dist"), str(HERE)]
encode_rgba = importlib.import_module("bounded_encoder").encode_rgba

lib.texture_pack_bc3.argtypes = [
    C.POINTER(Desc),
    C.c_void_p,
    C.c_uint32,
    C.c_void_p,
    C.c_uint32,
]
fixtures = []
for width, height in [(1, 1), (3, 5), (4, 4), (17, 9), (65, 33), (68, 65)]:
    for pattern in ("solid", "gradient", "alpha", "noise"):
        rgba = bytearray()
        for y in range(height):
            for x in range(width):
                if pattern == "solid":
                    pixel = [96, 128, 240, 255]
                elif pattern == "gradient":
                    pixel = [
                        (x * 11) % 256,
                        (y * 29) % 256,
                        (x * 7 + y * 13) % 256,
                        255,
                    ]
                elif pattern == "alpha":
                    pixel = [0, 0, 0, (x * 37 + y * 17) % 256]
                else:
                    pixel = [
                        ((x * 193 + y * 173 + c * 61) ^ (x * y * 29 + c * 41)) % 256
                        for c in range(4)
                    ]
                rgba.extend(pixel)
        # 未知头字段刻意非零，原生打包必须保留它们。
        # Unknown header fields intentionally nonzero: native pack must preserve them.
        header = bytearray((i * 7 + 3) % 256 for i in range(40))
        struct.pack_into("<4sHHII", header, 0, b"SpA1", 61, 5, 5, 7)
        struct.pack_into("<H", header, 20, 1)
        header[23] = len(fixtures) % 6
        linear = original["bc3"](Image.frombytes("RGBA", (width, height), bytes(rgba)))
        assert encode_rgba(rgba, width, height) == linear, (width, height, pattern)
        sprite = original["pack"](lib, header, linear, width, height, width)
        d = original["desc"](header, width, height)
        src = C.create_string_buffer(linear, len(linear))
        dst = C.create_string_buffer(len(sprite) - 40)
        assert lib.texture_pack_bc3(C.byref(d), src, len(linear), dst, len(dst)) == 0
        assert dst.raw == sprite[40:]
        assert lib.texture_pack_bc3(C.byref(d), src, len(linear) - 1, dst, len(dst)) == -5
        fixtures.append(
            {
                "width": width,
                "height": height,
                "pattern": pattern,
                **{
                    key: base64.b64encode(value).decode()
                    for key, value in [
                        ("templateHeader", header),
                        ("rgba", rgba),
                        ("linearBC3", linear),
                        ("sprite", sprite),
                    ]
                },
            }
        )
# C 边界必须在接触调用方缓冲区前拒绝格式错误的调用。
# The C boundary must reject malformed calls before touching caller buffers.
for changes, code in [
    ({"format": 0x41}, -3),
    ({"levels": 2}, -3),
    ({"flags": 0x10}, -3),
    ({"width": 2049, "height": 2049}, -4),
    ({"width": 8193}, -1),
]:
    invalid = Desc(1, 0x44, 0, 0, 16, 16, 1, 1, 0, 1)
    for key, value in changes.items():
        setattr(invalid, key, value)
    assert lib.texture_pack_bc3(C.byref(invalid), None, 0, None, 0) == code
(HERE / ".build/encoder-fixtures.json").write_text(json.dumps(fixtures, separators=(",", ":")))
print(
    f"PASS original full-image vs bounded band encoding and C/native swizzle: {len(fixtures)} synthetic cases"
)
