"""窄范围像素一致性样例，不涉及游戏资源或字体后端。
Narrow pixel equivalence fixtures, no game assets and no font backend involved."""

import base64
import hashlib
import json
import pathlib
import random
from PIL import Image, ImageFilter

assert Image.__version__ == "10.2.0", "Reference must be pinned Pillow 10.2.0"
rng = random.Random(461552)
cases = []


def encoded(im):
    return dict(
        width=im.width,
        height=im.height,
        channels=len(im.getbands()),
        data=base64.b64encode(im.tobytes()).decode(),
    )


def sample(mode, w, h):
    return Image.frombytes(mode, (w, h), rng.randbytes(w * h * len(mode)))


def add(op, src, result, args=None, **extra):
    cases.append(
        dict(
            op=op,
            src=encoded(src),
            args=args or [],
            expected=encoded(result) if isinstance(result, Image.Image) else result,
            **{k: encoded(v) if isinstance(v, Image.Image) else v for k, v in extra.items()},
        )
    )


for mode in ["L", "RGB", "RGBA"]:
    src = sample(mode, 11, 9)
    for box in [
        (-3, -2, 14, 12),
        (0, 0, 11, 9),
        (2, 3, 5, 8),
        (20, 20, 25, 24),
        (-20, -20, -15, -15),
    ]:
        x, y, r, b = box
        add("crop", src, src.crop(box), [x, y, r - x, b - y])
    for axis in [0, 1]:
        add(
            "flip",
            src,
            src.transpose(
                Image.Transpose.FLIP_LEFT_RIGHT if axis == 0 else Image.Transpose.FLIP_TOP_BOTTOM
            ),
            [axis],
        )
    for target in [1, 3, 4]:
        add("convert", src, src.convert({1: "L", 3: "RGB", 4: "RGBA"}[target]), [target])
    for c in range(len(mode)):
        add("channel", src, src.getchannel(c), [c])
    for x, y in [(-3, -2), (0, 0), (4, 5), (20, 22), (-20, -23)]:
        dst = sample(mode, 13, 10)
        for kind in [None, "L"] + (["source-alpha"] if mode == "RGBA" else []):
            mask = sample("L", 11, 9) if kind == "L" else None
            result = dst.copy()
            result.paste(src, (x, y), src if kind == "source-alpha" else mask)
            add(
                "paste",
                src,
                result,
                [x, y],
                dst=dst,
                mask=mask if kind == "L" else kind,
            )
    for wh in [
        (1, 1),
        (1, 9),
        (11, 1),
        (11, 9),
        (22, 18),
        (7, 5),
        (3, 2),
        (9, 7),
        (27, 13),
        (2, 20),
        (28, 2),
    ]:
        add("resizeLanczos", src, src.resize(wh, Image.Resampling.LANCZOS), list(wh))
    for w, h, outw, outh in [
        (1, 1, 9, 7),
        (1, 17, 8, 13),
        (19, 1, 13, 8),
        (97, 37, 5, 3),
        (113, 41, 173, 17),
        (257, 263, 128, 128),
        (119, 114, 114, 109),
        (152, 64, 92, 64),
    ]:
        src2 = sample(mode, w, h)
        add(
            "resizeLanczos",
            src2,
            src2.resize((outw, outh), Image.Resampling.LANCZOS),
            [outw, outh],
        )
for w, h in [(1, 1), (3, 2), (7, 9), (23, 17)]:
    src = sample("L", w, h)
    for size in [3, 5]:
        add("maxFilter", src, src.filter(ImageFilter.MaxFilter(size)), [size])
    lut = [v // 2 for v in range(256)]
    add("point", src, src.point(lut), lut=lut)
    lut = [255 if v > 24 else 0 for v in range(256)]
    add("point", src, src.point(lut), lut=lut)
for i in range(12):
    src = sample("RGBA", 17, 13)
    dst = sample("RGBA", 19, 11)
    x, y = [(-2, -3), (0, 0), (7, 5), (25, 30)][i % 4]
    result = dst.copy()
    result.alpha_composite(src, (x, y))
    add("alphaComposite", src, result, [x, y], dst=dst)
    mask = sample("L", 17, 13)
    result = src.copy()
    result.putalpha(mask)
    add("putAlpha", src, result, mask=mask)
# 覆盖每个源/目标 alpha 对，独立变化隐藏值及颜色值。
# Every source/destination alpha pair, independently varying hidden/color values.
a = bytearray(rng.randbytes(256 * 256 * 4))
b = bytearray(rng.randbytes(256 * 256 * 4))
for x in range(256):
    for y in range(256):
        a[(y * 256 + x) * 4 + 3] = x
        b[(y * 256 + x) * 4 + 3] = y
src = Image.frombytes("RGBA", (256, 256), bytes(a))
dst = Image.frombytes("RGBA", (256, 256), bytes(b))
add("alphaComposite", src, Image.alpha_composite(dst, src), [0, 0], dst=dst)
for mode in ["L", "RGB", "RGBA"]:
    for i in range(3):
        src = Image.new(mode, (7, 9))
        src.putpixel((2, 3), tuple([100] * len(mode)) if len(mode) > 1 else 100) if i else None
        if mode == "RGBA" and i == 2:
            src.putpixel((2, 3), (100, 99, 23, 0))
        for alpha in [True, False]:
            add("bbox", src, src.getbbox(alpha_only=alpha), [alpha])
root = pathlib.Path(__file__).parent
raw = json.dumps(cases, separators=(",", ":")).encode()
(root / "fixtures.json").write_bytes(raw)
(root / "fixtures-meta.json").write_text(
    json.dumps(
        dict(
            pillow=Image.__version__,
            cases=len(cases),
            sha256=hashlib.sha256(raw).hexdigest(),
        ),
        indent=2,
    )
    + "\n"
)
print(f"{len(cases)} pinned Pillow fixtures; {len(raw)} bytes")
