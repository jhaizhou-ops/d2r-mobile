"""有界浏览器转换，输入/输出均为字节，不提供目录、字体或网络 API。
Bounded browser transforms. Bytes in / bytes out; no directory, font or network API."""

import contextlib
import io
import struct
import types
from PIL import Image
import native_functions as native

MAX_PIXELS = 4_194_304
MAX_BYTES = 20 * 1024 * 1024


def sprite_v31(data):
    if len(data) < 40 or data[:4] != b"SpA1":
        raise ValueError("Invalid SpA1 header")
    version, frame_width, width, height = struct.unpack_from("<HHII", data, 4)
    if version == 61:
        raise ValueError("UNSUPPORTED_V61: NVN deswizzle/BC3 bridge is not implemented")
    if version != 31:
        raise ValueError("Unsupported sprite version")
    if not width or not height or width * height > MAX_PIXELS:
        raise ValueError("Invalid or oversized dimensions")
    if not frame_width or width % frame_width:
        raise ValueError("Invalid frame width")
    if len(data) != 40 + width * height * 4:
        raise ValueError("Truncated sprite or trailing payload")
    if struct.unpack_from("<II", data, 32) != (width * height * 4, 4):
        raise ValueError("Invalid RGBA byte count or stride")
    return Image.frombytes("RGBA", (width, height), data[40:]), frame_width


def encode_v31(im):
    # 使用 gen_romfs_overlay.py 的精确头表达式，只支持 RGBA v31。
    # Exact header expression in gen_romfs_overlay.py; only RGBA v31 supported.
    w, h = im.size
    if w > 65535:
        raise ValueError("Frame width exceeds v31 uint16")
    return (
        b"SpA1"
        + struct.pack("<HHIIIIII", 31, w, w, h, 0, 1, 0, 0)
        + struct.pack("<II", w * h * 4, 4)
        + im.tobytes()
    )


def execute(operation, payload, options):
    limit = (
        {
            "overlay-recipe": 128,
            "hud-v4": 64,
            "casc-overlay": 64,
        }.get(operation, 20)
        * 1024
        * 1024
    )
    if len(payload) > limit:
        raise ValueError("Input exceeds operation byte limit")
    if operation == "vault-table":
        from vault import generate

        return generate(payload, options)
    if operation == "remaining-art":
        from remaining_art import generate

        return generate(payload, options)
    if operation == "overlay-recipe":
        from overlay_recipe import generate

        return generate(payload, options)
    if operation == "mercenary-art":
        from mercenary import generate

        return generate(payload, options)
    if operation == "casc-overlay":
        from casc_overlay import generate

        return generate(payload, options)
    if operation == "map-goal":
        from map_goal import generate

        return generate(payload, options)
    if operation == "hud-v4":
        from hud import generate

        return generate(payload, options)
    if operation == "native-art":
        from native_art import generate

        return generate(payload, options)
    if operation == "derived-tables":
        from derived import generate

        return generate(payload, options)
    if operation == "image-default-frame":
        im, fw = sprite_v31(payload)
        index = options.get("frame", 0)
        if type(index) is not int or not 0 <= index < im.width // fw:
            raise ValueError("Frame index out of range")
        # 原始 romfs_art.frame 裁剪，然后使用未改动的 gen_hud_v4.dflt。
        # Original romfs_art.frame crop, then unmodified gen_hud_v4.dflt.
        im = native.dflt(im.crop((index * fw, 0, (index + 1) * fw, im.height)))
        png = io.BytesIO()
        im.save(png, format="PNG")
        return {
            "width": im.width,
            "height": im.height,
            "files": {
                "frame.rgba": im.tobytes(),
                "frame.png": png.getvalue(),
                "frame.sprite": encode_v31(im),
            },
            "versions": {"pillow": Image.__version__},
        }
    if operation == "experience-table":
        # 以有界内存读取器运行原始 main 主体，不导入原生命令行模块。
        # Run the original main body with a bounded in-memory reader, not native CLI imports.
        native.romfs_art = types.SimpleNamespace(read=lambda root, path: payload)
        output = io.StringIO()
        try:
            with contextlib.redirect_stdout(output):
                native.native_experience_main(None)
        except SystemExit as exc:
            raise ValueError(str(exc)) from exc
        finally:
            del native.romfs_art
        return {"files": {"dm_hud_exp.h": output.getvalue().encode("utf-8")}}
    if operation == "overlay-paths":
        import json

        paths = json.loads(payload.decode("utf-8"))
        if not isinstance(paths, list) or len(paths) > 4096:
            raise ValueError("Expected at most 4096 logical paths")
        result = []
        for path in paths:
            if (
                not isinstance(path, str)
                or len(path) > 1024
                or "\\" in path
                or ":" in path
                or any(p in ("", ".", "..") for p in path.split("/"))
            ):
                raise ValueError("Unsafe logical path")
            path = path.lower()
            result.append({"source": path, "target": native.touch_path(path)})
        return {"files": {"overlay-paths.json": json.dumps(result, separators=(",", ":")).encode()}}
    raise ValueError("Unsupported asset operation")
