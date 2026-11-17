"""完整 Switch 模式 HUD，使用显式解码图像及归档字体字节。
Complete Switch-mode HUD with explicit decoded images and archive font bytes."""

import base64
import hashlib
import io
import json
from pathlib import Path
import tempfile
import types

from PIL import Image, ImageFont, features
import hud_design_original as design
import hud_original as original
import hud_support_original as support

FONTS = ("blizzardglobal-v5_81.ttf", "exocetblizzardot-medium.otf")
IMAGE_NAMES = ("front_panel", "levelupbutton", "greater_healing_potion")
RELEASE_REVISION = "b811b90172f4eb40ec1bc89d903308e0c5ab88d0"
UPSTREAM_REVISION = "e3756f8942b40eba6c851e881828a5b1bda17fd6"


def require(condition, code):
    if not condition:
        raise ValueError(code)


def generate(payload, options):
    require(len(payload) <= 64 * 1024 * 1024, "HUD_INPUT_LIMIT")
    bundle = json.loads(payload.decode())
    require(options.get("mode") == "switch-upscaled", "HUD_MODE_REQUIRED")
    revision = options.get("revision", RELEASE_REVISION)
    require(
        revision in (RELEASE_REVISION, UPSTREAM_REVISION),
        "OVERLAY_RECIPE_REVISION",
    )
    metadata = bundle["metadata"]
    require(set(metadata) == {"patch", "i18n", "manifest"}, "HUD_METADATA_SET")
    pins = options.get("metadataSha256", {})
    require(set(pins) == set(metadata), "HUD_TRUST_REQUIRED")
    for name, text in metadata.items():
        require(
            isinstance(text, str) and len(text.encode()) <= 2 * 1024 * 1024,
            "HUD_METADATA_LIMIT",
        )
        require(hashlib.sha256(text.encode()).hexdigest() == pins[name], "HUD_METADATA_HASH")
    require(set(bundle["images"]) == set(IMAGE_NAMES), "HUD_IMAGE_SET")
    require(set(bundle["fonts"]) == set(FONTS), "HUD_FONT_SET")
    manifest = json.loads(metadata["manifest"])
    atlases = [a for a in manifest["assets"] if a["asset"].startswith("dm_native_skill_")]
    require(
        {a["asset"] for a in atlases} == {f"dm_native_skill_{i}.png" for i in range(9)}
        and len(atlases) == 9,
        "HUD_ATLAS_SET",
    )
    require(set(bundle["atlases"]) == {a["asset"] for a in atlases}, "HUD_ATLAS_SET")
    images, fonts = {}, {}
    pixels = 0
    for name, value in bundle["images"].items():
        w, h = value["size"]
        require(type(w) is int and type(h) is int and w > 0 and h > 0, "HUD_DIMENSIONS")
        pixels += w * h
        require(pixels <= 4194304, "HUD_PIXEL_LIMIT")
        raw = base64.b64decode(value["rgba"], validate=True)
        require(len(raw) == w * h * 4, "HUD_RGBA_SIZE")
        images[name] = Image.frombytes("RGBA", (w, h), raw)
    for name, value in bundle["fonts"].items():
        fonts[name] = base64.b64decode(value, validate=True)
        require(0 < len(fonts[name]) <= 16 * 1024 * 1024, "HUD_FONT_LIMIT")
    layout = bundle["layout"]
    require(
        isinstance(layout, str) and len(layout.encode()) <= 2 * 1024 * 1024,
        "HUD_LAYOUT_LIMIT",
    )

    class Source:
        pc = None

        def img(self, name):
            require(name in images, "HUD_UNDECLARED_IMAGE")
            return images[name].resize(design.ART[name][1], Image.Resampling.LANCZOS)

        def font(self, name, size):
            require(name in fonts and 1 <= size <= 256, "HUD_UNDECLARED_FONT")
            return ImageFont.truetype(io.BytesIO(fonts[name]), size)

    with tempfile.TemporaryDirectory(prefix="hud-") as tmp:
        root = Path(tmp)
        tools = root / "tools"
        art = root / "Sources/TouchArt"
        out = root / "out"
        for directory in (tools, art, out):
            directory.mkdir(parents=True)
        (tools / "touchrectv1_patch.json").write_text(metadata["patch"])
        (tools / "ui_text.tsv").write_text(metadata["i18n"])
        (art / "native_art_manifest.json").write_text(metadata["manifest"])
        for a in atlases:
            raw = base64.b64decode(bundle["atlases"][a["asset"]], validate=True)
            require(
                hashlib.sha256(raw).hexdigest() == a["decoded_png_sha256"],
                "HUD_ATLAS_HASH",
            )
            with Image.open(io.BytesIO(raw)) as image:
                require(list(image.size) == a["atlas_px"], "HUD_ATLAS_DIMENSIONS")
                pixels += image.width * image.height
                require(pixels <= 4194304, "HUD_PIXEL_LIMIT")
            (art / a["asset"]).write_bytes(raw)
        original.hud_v4_design = design
        original.layout_patch = support
        original.dm_i18n = types.SimpleNamespace(
            FMT=support.FMT,
            rows=lambda: [row for row in support.rows()],
        )
        original.romfs_art = types.SimpleNamespace(
            read=lambda _, p: layout.encode()
            if p == "global/ui/Layouts/Controller/HUDPanelHD.json"
            else (_ for _ in ()).throw(ValueError("HUD_UNDECLARED_READ"))
        )
        support.TABLE = str(tools / "ui_text.tsv")
        try:
            original.generate(None, str(out), str(tools), Source())
            files = {str(p.relative_to(out)): p.read_bytes() for p in out.rglob("*") if p.is_file()}
            require(
                len(files) <= 512 and sum(map(len, files.values())) <= 32 * 1024 * 1024,
                "HUD_OUTPUT_LIMIT",
            )
            # 每个渲染声明且有墨迹的字形都必须存在，不静默跳过任何输出。
            # Every render-declared glyph with ink must exist, and no output is silently skipped.
            ui = json.loads(files["hud_v4_ui/ui.json"])
            expected = {
                "base.png",
                "hud.json",
                "hud_v4_ui/ui.json",
                "hud_v4_ui/slot.png",
                "hud_v4_ui/base_full.png",
            }
            expected.update(f"hud_v4_ui/lvl_{i}.png" for i in (0, 1))
            expected.update(f"hud_v4_ui/lvl_d{i}.png" for i in (0, 1))
            expected.update(f"hud_v4_ui/diff_{i}.png" for i in range(1, 9))
            expected.update("hud_v4_ui/sil_" + a["asset"] for a in atlases)
            expected.update(
                "hud_v4_ui/lbl_" + n + ".png" for n in ["main"] + [f"f{i}" for i in range(1, 9)]
            )
            for key in ("lv", "badge", "vital"):
                expected.update(
                    "hud_v4_ui/" + g["file"] for g in ui[key]["glyphs"].values() if g["file"]
                )
            require(set(files) == expected, "HUD_OUTPUT_SET")
            return {
                "files": files,
                "mode": "switch-upscaled",
                "backend": {
                    "pillow": Image.__version__,
                    "freetype": features.version("freetype2"),
                    "raqm": features.check("raqm"),
                },
            }
        except SystemExit as error:
            raise ValueError(str(error)) from error
        finally:
            del (
                original.hud_v4_design,
                original.layout_patch,
                original.dm_i18n,
                original.romfs_art,
                support.TABLE,
            )
            for image in images.values():
                image.close()
