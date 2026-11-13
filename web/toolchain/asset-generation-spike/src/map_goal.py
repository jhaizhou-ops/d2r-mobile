"""有界地图目标边界，按源码版本设定清单门槛，仅使用当前名称。
Bounded map-goal boundary; source-revision-gated inventory, current names only."""

import base64
import io
import json
from pathlib import Path
import tempfile
import types

from PIL import Image, ImageFont, features
import map_goal_original as original

IMAGE_PATHS = {
    "symbols": "hd/global/ui/PANEL/Quest_Log/TalRashaSymbol.sprite",
    "units": "hd/global/ui/AUTOMAP/Units.sprite",
}
RELEASE_REVISION = "b811b90172f4eb40ec1bc89d903308e0c5ab88d0"
UPSTREAM_REVISION = "e3756f8942b40eba6c851e881828a5b1bda17fd6"


def require(condition, code):
    if not condition:
        raise ValueError(code)


def generate(payload, options):
    require(len(payload) <= 20 * 1024 * 1024, "MAP_GOAL_INPUT_LIMIT")
    require(
        set(options) in ({"names"}, {"names", "revision"}) and options.get("names") == "current",
        "MAP_GOAL_CURRENT_NAMES_REQUIRED",
    )
    revision = options.get("revision", RELEASE_REVISION)
    require(
        revision in (RELEASE_REVISION, UPSTREAM_REVISION),
        "OVERLAY_RECIPE_REVISION",
    )
    languages = ("zhCN", "enUS", "esES", "esMX", "ruRU", "koKR")
    bundle = json.loads(payload.decode())
    require(set(bundle) == {"images", "font", "strings"}, "MAP_GOAL_INPUT_SET")
    require(set(bundle["images"]) == set(IMAGE_PATHS), "MAP_GOAL_IMAGE_SET")
    tables = {name for name, _, _ in original.LABELS}
    require(set(bundle["strings"]) == tables, "MAP_GOAL_STRING_SET")
    strings = {}
    for name, text in bundle["strings"].items():
        require(
            isinstance(text, str) and len(text.encode()) <= 2 * 1024 * 1024,
            "MAP_GOAL_STRING_LIMIT",
        )
        rows = json.loads(text.lstrip("\ufeff"))
        require(isinstance(rows, list) and len(rows) <= 20000, "MAP_GOAL_STRING_ROWS")
        indexed = {}
        for row in rows:
            require(
                isinstance(row, dict) and isinstance(row.get("Key"), str),
                "MAP_GOAL_STRING_ROW",
            )
            require(row["Key"] not in indexed, "MAP_GOAL_DUPLICATE_KEY")
            indexed[row["Key"]] = row
        for table, key, _ in original.LABELS:
            if table == name:
                require(key in indexed, "MAP_GOAL_MISSING_KEY")
                for lang in languages:
                    value = indexed[key].get(lang)
                    require(
                        isinstance(value, str)
                        and 0 < len(value) <= 256
                        and not any(ord(c) < 32 for c in value),
                        "MAP_GOAL_LABEL_LIMIT",
                    )
        strings[name] = text.encode()
    images = {}
    try:
        pixels = 0
        for name, value in bundle["images"].items():
            w, h = value["size"]
            fw, frames = value["frameWidth"], value["frames"]
            require(
                all(type(v) is int and v > 0 for v in (w, h, fw, frames)),
                "MAP_GOAL_DIMENSIONS",
            )
            require(
                frames == (7 if name == "symbols" else 8) and w % frames == 0 and fw <= w // frames,
                "MAP_GOAL_FRAME_LAYOUT",
            )
            pixels += w * h
            require(pixels <= 2097152, "MAP_GOAL_PIXEL_LIMIT")
            rgba = base64.b64decode(value["rgba"], validate=True)
            require(len(rgba) == w * h * 4, "MAP_GOAL_RGBA_SIZE")
            images[IMAGE_PATHS[name]] = (
                Image.frombytes("RGBA", (w, h), rgba),
                fw,
                frames,
                w // frames,
            )
        font = base64.b64decode(bundle["font"], validate=True)
        require(0 < len(font) <= 9 * 1024 * 1024, "MAP_GOAL_FONT_LIMIT")
        # 进入未修改的原始渲染器前验证分配几何。
        # Validate allocation geometry before entering the unchanged original renderer.
        measured_font = ImageFont.truetype(io.BytesIO(font), original.TEXT_PX)
        for text in strings.values():
            rows = {r["Key"]: r for r in json.loads(text.decode("utf-8-sig"))}
            for _, key, _ in original.LABELS:
                if key in rows:
                    for lang in languages:
                        width = measured_font.getlength(rows[key][lang])
                        require(
                            0 <= width <= 8192 - 2 * original.PAD_X,
                            "MAP_GOAL_RENDER_WIDTH",
                        )
        with tempfile.TemporaryDirectory(prefix="map-goal-") as tmp:
            root = Path(tmp)
            original.OUT = str(root / "out")
            original.HERE = str(
                root
            )  # 不使用可选的私有 annot/oldnames.tsv。 | No optional private annot/oldnames.tsv.
            original.ROMFS = None
            original.romfs_art = types.SimpleNamespace(
                read=lambda _, p: strings[
                    p.removeprefix("local/lng/strings/").removesuffix(".json")
                ],
                font=lambda _, name: io.BytesIO(font)
                if name == "BlizzardGlobal-V5_81.ttf"
                else require(False, "MAP_GOAL_FONT_NAME"),
            )
            old_sprite = original.sprite_png
            original.sprite_png = lambda path: images[path]
            try:
                original.main(True)
            finally:
                original.sprite_png = old_sprite
                del original.romfs_art, original.OUT, original.HERE, original.ROMFS
            expected = {"mapgoal.json", "cross.png"} | {f"sym_{i}.png" for i in range(7)}
            expected |= {
                f"label_{i}{suffix}.png"
                for i in range(len(original.LABELS))
                for suffix in ("", "_en", "_es", "_mx", "_ru", "_ko")
            }
            paths = {p.name: p for p in (root / "out").iterdir() if p.is_file()}
            require(
                set(paths) == expected and len(expected) == 195,
                "MAP_GOAL_OUTPUT_SET",
            )
            require(
                sum(p.stat().st_size for p in paths.values()) <= 16 * 1024 * 1024,
                "MAP_GOAL_OUTPUT_LIMIT",
            )
            return {
                "files": {name: p.read_bytes() for name, p in paths.items()},
                "backend": {
                    "pillow": Image.__version__,
                    "freetype": features.version("freetype2"),
                    "raqm": features.check("raqm"),
                },
            }
    finally:
        for image, *_ in images.values():
            image.close()
