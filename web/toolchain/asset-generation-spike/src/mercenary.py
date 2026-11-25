"""对三个提供的解码图集执行有界原始佣兵合成。
Bounded original mercenary composition over three supplied decoded atlases."""

import base64
import io
import json
import struct
import types

from PIL import Image
import mercenary_original as original

INPUTS = {
    "hirelinginventory/v2/hirelinginventorybg": (652, 704),
    "inventorypanel/v2/inventorybg_classic": (507, 700),
    "mainpanelbg": (2888, 881),
}


def generate(payload, options):
    if len(payload) > 20 * 1024 * 1024 or options:
        raise ValueError("MERCENARY_INPUT_LIMIT")
    images = json.loads(payload.decode())
    if not isinstance(images, dict) or set(images) != set(INPUTS):
        raise ValueError("MERCENARY_INPUT_SET")
    decoded, headers = {}, {}
    try:
        for name, size in INPUTS.items():
            entry = images[name]
            if set(entry) != {"header", "rgba"}:
                raise ValueError("MERCENARY_INPUT_FIELDS")
            header = base64.b64decode(entry["header"], validate=True)
            rgba = base64.b64decode(entry["rgba"], validate=True)
            if len(header) != 40 or header[:4] != b"SpA1":
                raise ValueError("MERCENARY_HEADER")
            ver, fw, width, height = struct.unpack_from("<HHII", header, 4)
            if (
                ver != 61
                or (width, height) != size
                or (name == "hirelinginventory/v2/hirelinginventorybg" and fw != 652)
                or len(rgba) != width * height * 4
            ):
                raise ValueError("MERCENARY_DIMENSIONS")
            headers[name] = header
            decoded[header] = Image.frombytes("RGBA", size, rgba)
        original.GW = types.SimpleNamespace(
            lib=lambda: None,
            orig=lambda _, name: headers[name],
            decode=lambda _, header: decoded[header].copy(),
        )
        result = original.compose(None)
        png = io.BytesIO()
        result.save(png, format="PNG")
        return {
            "width": 652,
            "height": 704,
            "files": {
                "mercenary.rgba": result.tobytes(),
                "HirelingInventoryBG.png": png.getvalue(),
            },
        }
    finally:
        if hasattr(original, "GW"):
            del original.GW
        for image in decoded.values():
            image.close()
