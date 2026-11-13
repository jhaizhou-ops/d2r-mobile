"""固定原生仓库、储藏库及袋子美术的有界解码图像边界。
Bounded decoded-image boundary for pinned native stash, vault and pouch art."""

import base64
import io
import json

from PIL import Image
import remaining_art_original as original
import trove_original as trove


def generate(payload, options):
    if len(payload) > 20 * 1024 * 1024 or not isinstance(options, dict):
        raise ValueError("REMAINING_ART_INPUT_LIMIT")
    if set(options) != {"kind", "name"}:
        raise ValueError("REMAINING_ART_OPTIONS")
    kind, name = options["kind"], options["name"]
    expected = {
        "stash": {"image", "personal"},
        "trove": {"gems", "runes", "cons", "character"},
        "advanced": {"image"},
        "vault": {"image"},
        "pouch": {"image"},
    }
    if kind not in expected:
        raise ValueError("REMAINING_ART_KIND")
    data = json.loads(payload.decode())
    if not isinstance(data, dict) or set(data) != expected[kind]:
        raise ValueError("REMAINING_ART_INPUT_SET")
    images = {}
    result = None
    try:
        for key, entry in data.items():
            if not isinstance(entry, dict) or set(entry) != {"width", "height", "rgba"}:
                raise ValueError("REMAINING_ART_FIELDS")
            w, h = entry["width"], entry["height"]
            if type(w) is not int or type(h) is not int or not 1 <= w <= 1640 or not 1 <= h <= 1024:
                raise ValueError("REMAINING_ART_GEOMETRY")
            raw = base64.b64decode(entry["rgba"], validate=True)
            if len(raw) != w * h * 4:
                raise ValueError("REMAINING_ART_RGBA")
            images[key] = Image.frombytes("RGBA", (w, h), raw)
        image = images.get("image")
        if kind == "stash":
            if name not in original.SPEC or image.width != 652 or images["personal"].width != 652:
                raise ValueError("REMAINING_ART_STASH_GEOMETRY")
            if image.height < 644 or images["personal"].height < 644:
                raise ValueError("REMAINING_ART_STASH_HEIGHT")
            stone = images["personal"].crop((248, 533, 346, 577))
            try:
                result = original.widen_bg(image, stone, **original.SPEC[name])
            finally:
                stone.close()
        elif kind == "trove":
            if name != "AdvancedStash_Gems_Console":
                raise ValueError("REMAINING_ART_TROVE_NAME")
            built = trove.build(
                images["gems"], images["runes"], images["cons"], images["character"]
            )
            result = Image.new("RGBA", (848, images["gems"].height))
            result.paste(built, (trove.X0, 0))
            built.close()
        elif kind == "advanced":
            if name not in {
                "AdvancedStash_Consumables_Console",
                "AdvancedStash_Runes_Console",
            } or image.size != (652, 649):
                raise ValueError("REMAINING_ART_ADVANCED_GEOMETRY")
            result = Image.new("RGBA", (848, image.height))
            result.paste(image, (98, 0))
        elif kind == "vault":
            if name != "VaultBottom" or image.width != 848 or image.height < 644:
                raise ValueError("REMAINING_ART_VAULT_GEOMETRY")
            result = original.vault_cover(image)
        elif kind == "pouch":
            if name != "PouchBox" or image.width not in {507, 508} or image.height != 700:
                raise ValueError("REMAINING_ART_POUCH_GEOMETRY")
            result = original.pouch_box(image)
        files = {"image.rgba": result.tobytes()}
        if kind == "vault":
            png = io.BytesIO()
            result.save(png, format="PNG")
            files["VaultBottom.png"] = png.getvalue()
        return {"width": result.width, "height": result.height, "files": files}
    except SystemExit as exc:
        raise ValueError("REMAINING_ART_SOURCE_GUARD") from exc
    finally:
        if result is not None:
            result.close()
        for image in images.values():
            image.close()
