"""针对实际打包 WASM Pillow 样例像素的窄范围原生 ABI 检查，验证移植语义，不验证尚未构建的 WASM 产物或游戏字体。
Narrow native-ABI check against actual bundled WASM Pillow fixture pixels.
This validates the port's semantics, NOT the unbuilt WASM artifact or game fonts.
"""

import base64
import ctypes as C
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fixture = json.loads((root / "test/fixtures.json").read_text())
assert fixture["backend"] == dict(
    pillow="10.2.0", freetype="2.6.0", layout="BASIC", raqm=False, pyodide="0.27.7"
)
lib = C.CDLL(str(root / "dist/font-rasterizer.so"))
for name in ("font_input", "font_mask", "font_canvas"):
    getattr(lib, name).restype = C.POINTER(C.c_ubyte)
lib.font_text.restype = C.POINTER(C.c_uint32)
lib.font_info.restype = C.POINTER(C.c_int32)
lib.font_open.argtypes = [C.c_uint32, C.c_float]
lib.font_measure.argtypes = [C.c_uint32, C.c_int, C.c_int]
lib.font_render.argtypes = [C.c_uint32, C.c_int, C.c_int, C.c_float, C.c_float]
lib.font_draw.argtypes = [
    C.c_uint32,
    C.c_int,
    C.c_int,
    C.c_double,
    C.c_double,
    C.c_int,
    C.c_int,
    C.c_int,
    C.c_uint32,
    C.c_uint32,
]
assert lib.font_version() == 20600
anchors = dict(la=0, ls=1, mb=2, mm=3)


def check(code):
    assert code == 0, (code, lib.font_error(), lib.font_ft_error(), case)


def packed(value):
    if isinstance(value, int):
        return value
    return sum(c << (i * 8) for i, c in enumerate(value))


def equal(actual, expected, label):
    if actual != expected:
        if isinstance(actual, bytes):
            n = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), -1)
            raise AssertionError(
                (
                    index,
                    label,
                    "firstDifference",
                    n,
                    len(actual),
                    len(expected),
                    actual[n : n + 16],
                    expected[n : n + 16],
                    {k: v for k, v in case.items() if k not in ("mask", "pixels", "before")},
                )
            )
        raise AssertionError((index, label, actual, expected))


opened = None
for index, case in enumerate(fixture["cases"]):
    key = case["font"], case["size"]
    if opened != key:
        lib.font_close()
        font = fixture["fonts"][case["font"]]
        raw = Path(font["path"]).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == font["sha256"]
        C.memmove(lib.font_input(), raw, len(raw))
        check(lib.font_open(len(raw), case["size"]))
        opened = key
    for i, ch in enumerate(case["text"]):
        lib.font_text()[i] = ord(ch)
    args = len(case["text"]), anchors[case["anchor"]], case["stroke"]
    if case["op"] == "mask":
        check(lib.font_measure(*args))
        w, h, x, y, advance, asc, desc = lib.font_info()[:7]
        equal([x, y, x + w, y + h], case["bbox"], "bbox")
        equal(advance / 64, case["length"], "length")
        equal([asc, desc], case["metrics"], "metrics")
        check(lib.font_render(*args, *case["start"]))
        w, h, x, y = lib.font_info()[:4]
        equal([w, h], case["maskSize"], "mask size")
        equal([x, y], case["offset"], "mask offset")
        equal(
            C.string_at(lib.font_mask(), w * h),
            base64.b64decode(case["mask"]),
            "mask bytes",
        )
    else:
        raw = base64.b64decode(case["before"])
        C.memmove(lib.font_canvas(), raw, len(raw))
        check(
            lib.font_draw(
                *args,
                *case["pos"],
                case["width"],
                case["height"],
                case["channels"],
                packed(case["fill"]),
                packed(case["strokeFill"]),
            )
        )
        equal(
            C.string_at(lib.font_canvas(), len(raw)),
            base64.b64decode(case["pixels"]),
            "draw bytes",
        )
lib.font_close()
# 小范围拒绝检查，不是通用字体模糊测试。
# Small rejection checks, not a general font fuzzing campaign.
assert lib.font_measure(0, 0, 0) == 2
assert lib.font_open(0, 30) == 1
font = fixture["fonts"][0]
raw = Path(font["path"]).read_bytes()
C.memmove(lib.font_input(), raw, len(raw))
check(lib.font_open(len(raw), 30))
assert lib.font_measure(2049, 0, 0) == 1
assert lib.font_measure(0, 4, 0) == 1
assert lib.font_measure(0, 0, 3) == 1
assert lib.font_render(0, 0, 0, 1.0, 0.0) == 1
lib.font_text()[0] = 0xD800
assert lib.font_measure(1, 0, 0) == 1
lib.font_close()
print(
    json.dumps(
        dict(
            cases=len(fixture["cases"]),
            status="exact",
            comparison="native port vs bundled WASM Pillow10.2/FreeType2.6/BASIC",
            gameFontsVerified=False,
            wasmArtifactVerified=False,
        )
    )
)
