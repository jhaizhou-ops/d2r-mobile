"""显式类型数组内核的 Pillow 边界，不是 NumPy 兼容层。
Pillow boundary for explicit typed-array kernels; not a NumPy compatibility layer."""

from PIL import Image
from pyodide.ffi import to_js
import art_kernel_bridge as kernels


def _bytes(im, mode):
    return to_js(memoryview(im.convert(mode).tobytes()))


def _image(raw, size, mode="RGBA"):
    return Image.frombytes(mode, size, raw.to_py().tobytes())


def default_state(im):
    return _image(kernels.defaultState(_bytes(im, "RGBA"), *im.size), im.size)


def clear_circles(im, circles):
    return _image(kernels.clearCircles(_bytes(im, "RGBA"), *im.size, to_js(circles)), im.size)


def silhouette(im, frame_width, pitch):
    return _image(kernels.silhouette(_bytes(im, "RGBA"), *im.size, frame_width, pitch), im.size)


def metal(mask, outline):
    return _image(kernels.metal(_bytes(mask, "L"), _bytes(outline, "L"), *mask.size), mask.size)


def feather(size, n=2):
    return _image(kernels.feather(*size, n), size, "L")


def stats(im, boxes):
    pixels = b"".join(im.crop(box).convert("RGB").tobytes() for box in boxes)
    return kernels.rgbStats(to_js(memoryview(pixels))).to_py()


def stone(im, width, height, target, ox=90, oy=190):
    source = im.crop((ox, oy, ox + width, oy + height))
    return _image(
        kernels.stone(_bytes(source, "RGB"), width, height, to_js(target[0]), to_js(target[1])),
        (width, height),
    )


def vault_cover(im):
    # 填充裁剪刻意仍使用 Pillow，图像范围外的行透明。
    # Padded crop is deliberately still Pillow: out-of-image rows are transparent.
    result = _image(kernels.vaultCover(_bytes(im, "RGBA"), *im.size), im.size)
    eh = im.height - 516
    eh += -eh % 4
    return result.crop((0, 516, im.width, 516 + eh))
