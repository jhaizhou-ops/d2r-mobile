#!/usr/bin/env python3
"""仅用合成内容，编译未修改原生函数并与有界桥接比较。
Synthetic only. Compile unchanged native functions and compare bounded bridge."""

import base64
import ctypes as C
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = HERE / ".build"
BUILD.mkdir(exist_ok=True)
sources = [
    ROOT / "dm/gfx/nvn/nvn_tex.c",
    ROOT / "dm/gfx/tex/nvn_deswizzle.c",
    ROOT / "dm/gfx/tex/nvn_bcdec.c",
]
subprocess.run(
    [
        "cc",
        "-std=c11",
        "-O2",
        "-shared",
        "-fPIC",
        "-I" + str(ROOT / "dm/gfx/nvn"),
        "-I" + str(ROOT / "dm/gfx/tex"),
        str(HERE / "boundary.c"),
        *map(str, sources),
        "-o",
        str(BUILD / "native.so"),
    ],
    check=True,
)
lib = C.CDLL(str(BUILD / "native.so"))


class Desc(C.Structure):
    _fields_ = [
        (
            n,
            C.c_int if n in ("target", "format", "flags", "samples", "pkg_block_h") else C.c_uint32,
        )
        for n in (
            "target",
            "format",
            "flags",
            "samples",
            "width",
            "height",
            "depth",
            "levels",
            "stride",
            "pkg_block_h",
        )
    ]


class Layout(C.Structure):
    _fields_ = [
        ("size", C.c_uint64),
        ("layer_size", C.c_uint64),
        ("align", C.c_uint32),
        ("layers", C.c_uint32),
        ("bpb", C.c_uint32),
        ("tileH", C.c_uint),
        ("tileD", C.c_uint),
    ]


lib.nvn_tex_layout.argtypes = [C.POINTER(Desc), C.POINTER(Layout)]
lib.nvn_swizzle.argtypes = [
    C.POINTER(Desc),
    C.c_uint,
    C.c_uint,
    C.c_void_p,
    C.c_size_t,
    C.c_void_p,
]
lib.nvn_deswizzle.argtypes = [
    C.POINTER(Desc),
    C.c_uint,
    C.c_uint,
    C.c_void_p,
    C.c_void_p,
    C.c_size_t,
]
lib.nvn_bc_decode.argtypes = [
    C.c_uint32,
    C.c_void_p,
    C.c_size_t,
    C.c_uint32,
    C.c_uint32,
    C.c_void_p,
    C.c_size_t,
]
lib.nvn_bc_unpacked.argtypes = [C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(C.c_int)]
lib.texture_info.argtypes = [C.POINTER(Desc), C.c_uint, C.c_uint, C.POINTER(C.c_uint32)]
lib.texture_decode.argtypes = [
    C.POINTER(Desc),
    C.c_uint,
    C.c_uint,
    C.c_void_p,
    C.c_uint32,
    C.c_void_p,
    C.c_uint32,
]
fixtures = []
for fmt in range(0x41, 0x51):
    for w, h, levels, flags, pkg in [
        (1, 1, 1, 0, 1),
        (3, 5, 3, 0, 0),
        (17, 9, 5, 0, 4),
        (65, 129, 7, 0, 6),
        (7, 5, 1, 0x10, 0),
        (17, 9, 1, 0x50, 0),
        (19, 13, 1, 0x10, 0),
    ]:
        d = Desc(1, fmt, flags, 0, w, h, 1, levels, 0, pkg)
        if w == 19:
            d.stride = ((w + 3) // 4) * (
                8 if fmt in (0x41, 0x42, 0x45, 0x46, 0x49, 0x4A) else 16
            ) + 17
        layout = Layout()
        lib.nvn_tex_layout(C.byref(d), C.byref(layout))
        storage = C.create_string_buffer(layout.size)
        pf, bpp = C.c_uint32(), C.c_int()
        assert lib.nvn_bc_unpacked(fmt, C.byref(pf), C.byref(bpp)) == 1
        expected = []
        for level in range(levels):
            lw, lh = max(1, w >> level), max(1, h >> level)
            pitch, rows = ((lw + 3) // 4) * layout.bpb, (lh + 3) // 4
            raw = bytes(((i * 31 + fmt * 7 + level * 13) & 255) for i in range(pitch * rows))
            linear = C.create_string_buffer(raw, len(raw))
            if flags:
                alignment = 128 if flags & 0x40 else 32
                stride = d.stride or ((pitch + alignment - 1) & ~(alignment - 1))
                for y in range(rows):
                    C.memmove(
                        C.addressof(storage) + y * stride,
                        raw[y * pitch : (y + 1) * pitch],
                        pitch,
                    )
            else:
                assert lib.nvn_swizzle(C.byref(d), level, 0, linear, pitch, storage) == 0
                recovered = C.create_string_buffer(len(raw))
                assert lib.nvn_deswizzle(C.byref(d), level, 0, storage, recovered, pitch) == 0
                assert recovered.raw == raw
            pixels = C.create_string_buffer(lw * lh * bpp.value)
            assert lib.nvn_bc_decode(fmt, linear, pitch, lw, lh, pixels, lw * bpp.value) == 0
            expected.append(pixels.raw)
        for level, pixels in enumerate(expected):
            info = (C.c_uint32 * 8)()
            assert lib.texture_info(C.byref(d), level, 0, info) == 0
            assert info[0] == layout.size
            result = C.create_string_buffer(len(pixels))
            assert (
                lib.texture_decode(C.byref(d), level, 0, storage, layout.size, result, len(pixels))
                == 0
            )
            assert result.raw == pixels
            assert (
                lib.texture_decode(
                    C.byref(d), level, 0, storage, layout.size - 1, result, len(pixels)
                )
                == -5
            )
            fixtures.append(
                {
                    "descriptor": {k: getattr(d, k) for k, _ in Desc._fields_},
                    "level": level,
                    "info": list(info),
                    "source": base64.b64encode(storage.raw).decode(),
                    "pixels": base64.b64encode(pixels).decode(),
                }
            )
# 独立的已知值 BC1 参考，不仅与同一库比较一致性。
# Independent known-value BC1 oracle, not only parity against the same library.
red = C.create_string_buffer(bytes.fromhex("00f8000000000000"), 8)
pixels = C.create_string_buffer(64)
assert lib.nvn_bc_decode(0x41, red, 8, 4, 4, pixels, 16) == 0
assert pixels.raw == bytes([255, 0, 0, 255]) * 16
info = (C.c_uint32 * 8)()
for changes, code in [
    ({"format": 0x79}, -2),
    ({"width": 0}, -1),
    ({"width": 8193}, -1),
    ({"levels": 32}, -1),
    ({"pkg_block_h": 7}, -1),
    ({"target": 2}, -3),
    ({"depth": 2}, -3),
    ({"samples": 4}, -3),
    ({"flags": 0x10, "levels": 2}, -3),
    ({"width": 8192, "height": 8192}, -4),
    ({"flags": 0x10, "stride": 1}, -1),
]:
    d = Desc(1, 0x44, 0, 0, 16, 16, 1, 1, 0, 0)
    for key, value in changes.items():
        setattr(d, key, value)
    assert lib.texture_info(C.byref(d), 0, 0, info) == code, changes
(BUILD / "fixtures.json").write_text(json.dumps(fixtures, separators=(",", ":")))
print(
    f"PASS native independent layout/deswizzle/decode parity: {len(fixtures)} fixtures, malformed input checks, BC1 known-red oracle"
)
