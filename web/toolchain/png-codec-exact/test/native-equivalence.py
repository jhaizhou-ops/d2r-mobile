"""针对实际打包的 WASM Pillow PNG 参考进行窄范围原生构建检查。
Narrow native build check against the actual bundled WASM Pillow PNG oracle."""

import base64
import ctypes
import json
import pathlib
import struct
import zlib

root = pathlib.Path(__file__).resolve().parents[1]
lib = ctypes.CDLL(str(root / "dist/png_codec_native.so"))
lib.png_input.restype = ctypes.POINTER(ctypes.c_ubyte)
lib.png_output.restype = ctypes.POINTER(ctypes.c_ubyte)
lib.png_encode.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
lib.png_decode.argtypes = [ctypes.c_uint32]
lib.png_zlib_version.restype = ctypes.c_char_p
assert lib.png_zlib_version() == b"1.3.1"
fixture = json.loads((root / "test/fixtures.json").read_text())
assert fixture["backend"] == dict(
    pillow="10.2.0", pillowZlib="1.3.1", pyodide="0.27.7", genericZlib="1.2.13"
)
for i, case in enumerate(fixture["cases"]):
    raw = base64.b64decode(case["rgba"])
    png = base64.b64decode(case["png"])
    ctypes.memmove(lib.png_input(), raw, len(raw))
    n = lib.png_encode(case["width"], case["height"])
    assert n > 0, (i, lib.png_error())
    actual = ctypes.string_at(lib.png_output(), n)
    assert actual == png, (
        i,
        case["kind"],
        case["width"],
        case["height"],
        len(actual),
        len(png),
    )
    ctypes.memmove(lib.png_input(), png, len(png))
    n = lib.png_decode(len(png))
    assert n == len(raw) and ctypes.string_at(lib.png_output(), n) == raw, (
        i,
        lib.png_error(),
    )
    assert (lib.png_width(), lib.png_height()) == (case["width"], case["height"])
# 实际自编关闭按钮是配方解码的唯一固定源 PNG。
# Actual authored close button is the only fixed source PNG decoded by recipe.
close = (root.parents[2] / "dm/ios/NativeHUD/touch/close.png").read_bytes()
ctypes.memmove(lib.png_input(), close, len(close))
assert lib.png_decode(len(close)) == 24 * 33 * 4
# 检查无效签名、校验和、尾部字节、截断块及超大 IHDR。
# Invalid signature, checksum, trailing bytes, truncated chunk and oversized IHDR.
base = base64.b64decode(fixture["cases"][2]["png"])
bad = bytearray(base)
bad[45] ^= 1
oversized = bytearray(base)
oversized[16:20] = struct.pack(">I", 8193)
oversized[29:33] = struct.pack(">I", zlib.crc32(oversized[12:29]))
for data in [b"bad", bytes(bad), base + b"x", base[:-1], bytes(oversized)]:
    ctypes.memmove(lib.png_input(), data, len(data))
    assert lib.png_decode(len(data)) == -1
assert lib.png_encode(0, 1) == -1 and lib.png_encode(8192, 8192) == -1
print(
    f"Exact encoded bytes and decoded pixels: {len(fixture['cases'])} bundled Pyodide Pillow/zlib fixtures; fixed close-button decode; invalid input rejected."
)

lib.generic_zlib_version.restype = ctypes.c_char_p
assert lib.generic_zlib_version() == b"1.2.13"
for case in fixture["generic"]:
    n = case["length"]
    raw = (
        (bytes(range(251)) * (n // 251 + 1))[:n]
        if case["kind"] == "pattern"
        else base64.b64decode(case["raw"])
    )
    assert lib.zlib_begin(9) == 0
    chunks = []
    step = 1024 * 1024
    for off in range(0, max(1, len(raw)), step):
        part = raw[off : off + step]
        ctypes.memmove(lib.png_input(), part, len(part))
        size = lib.zlib_push(len(part), int(off + step >= len(raw)))
        assert size >= 0, case["length"]
        chunks.append(ctypes.string_at(lib.png_output(), size))
    assert b"".join(chunks) == base64.b64decode(case["compressed"]), case["length"]
assert lib.zlib_begin(6) == -1
print(
    f"Exact CPython zlib 1.2.13 streams: {len(fixture['generic'])} cases, including empty, BLTE boundaries, chunk boundaries and 16MiB limit input."
)
