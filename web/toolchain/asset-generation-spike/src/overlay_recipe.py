"""在有界内存输入上运行原始 iOS 覆盖配方，不运行原生进程。
Original iOS overlay recipe on bounded in-memory inputs, no native processes."""

import base64
import builtins
import contextlib
import hashlib
import importlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types

from PIL import Image
from overlay_recipe_sources import SOURCES, RECIPE

MAX_PAYLOAD = 128 * 1024 * 1024
MAX_ASSETS = 64 * 1024 * 1024
MAX_GENERATED = 48 * 1024 * 1024
MAX_OUTPUT = 128 * 1024 * 1024
GENERATED = (
    "NativeHUD/v4/base.png",
    "NativeHUD/v4/hud.json",
    "NativeHUD/v4/hud_v4_ui/ui.json",
    "NativeHUD/touch/fe_rightpanel.sprite",
    "NativeHUD/mercslots/HirelingInventoryBG.sprite",
    "NativeHUD/pouch/PouchBox.sprite",
    "NativeHUD/widestash/MainPanelBG_Wide.sprite",
    "NativeHUD/widestash/VaultBottom.sprite",
    *[
        f"NativeHUD/widestash/{n}_wide.sprite"
        for n in (
            "StashPanelBG",
            "StashPanelBGPersonal",
            "StashPanelBGShared",
            "AdvancedStash_Gems_Console",
            "AdvancedStash_Consumables_Console",
            "AdvancedStash_Runes_Console",
        )
    ],
)
MODULES = tuple(Path(p).stem for p in SOURCES if p.endswith(".py")) + (
    "gen_assets",
    "gen_merc_art",
    "romfs_art",
)


def require(value, message):
    if not value:
        raise ValueError(message)


def logical(path):
    require(isinstance(path, str), "OVERLAY_PATH")
    path = path.lower().replace("\\", "/").split(":", 1)[-1]
    require(
        path.startswith("data/")
        and len(path) <= 4096
        and all(p not in ("", ".", "..") for p in path.split("/")),
        "OVERLAY_PATH",
    )
    require(not any(ord(c) < 32 for c in path), "OVERLAY_PATH")
    return path


def decode_map(value, limit):
    require(isinstance(value, dict) and len(value) <= 8192, "OVERLAY_FILE_COUNT")
    result, size = {}, 0
    for key, encoded in value.items():
        require(
            isinstance(encoded, str) and len(encoded) <= 24 * 1024 * 1024,
            "OVERLAY_FILE_LIMIT",
        )
        data = base64.b64decode(encoded, validate=True)
        size += len(data)
        require(size <= limit and len(data) <= 16 * 1024 * 1024, "OVERLAY_BYTES_LIMIT")
        result[key] = data
    return result


def generate(payload, options):
    require(len(payload) <= MAX_PAYLOAD and options == {}, "OVERLAY_RECIPE_OPTIONS")
    value = json.loads(payload)
    require(set(value) == {"manifest", "assets", "generated"}, "OVERLAY_RECIPE_INPUT")
    manifest = value["manifest"]
    require(
        isinstance(manifest, str) and len(manifest.encode()) <= 16 * 1024 * 1024,
        "OVERLAY_MANIFEST_LIMIT",
    )
    assets = decode_map(value["assets"], MAX_ASSETS)
    require(all(logical(k) == k for k in assets), "OVERLAY_ASSET_PATH")
    generated = decode_map(value["generated"], MAX_GENERATED)
    require(set(generated) == set(GENERATED), "OVERLAY_GENERATED_ART_REQUIRED")
    with Image.open(io.BytesIO(generated["NativeHUD/v4/base.png"])) as image:
        require(
            image.format == "PNG" and image.width * image.height <= 4_194_304,
            "OVERLAY_HUD_IMAGE_LIMIT",
        )
    rows = []
    for line in manifest.splitlines():
        f = line.split("|")
        require(len(f) == 11, "OVERLAY_MANIFEST_ROW")
        logical(f[9])
        rows.append(f)
    require(len(rows) <= 262144, "OVERLAY_MANIFEST_ROWS")
    files, variants, output_size = {}, [], 0
    old_modules = {n: sys.modules.get(n) for n in MODULES}
    old_path, old_argv, old_env = sys.path[:], sys.argv[:], dict(os.environ)
    old_pixels = Image.MAX_IMAGE_PIXELS
    old_open, old_io_open = builtins.open, io.open
    try:
        with tempfile.TemporaryDirectory(prefix="native-overlay-") as tmp:
            root = Path(tmp)
            for name, encoded in SOURCES.items():
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(base64.b64decode(encoded, validate=True))
            tools = root / "dm/ios/tools"
            gen = root / "generated"
            for name, data in generated.items():
                p = gen / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(data)
            romfs = root / "romfs"
            (romfs / "data").mkdir(parents=True)
            (romfs / "data/build_manifest.psv").write_text(manifest)
            out = root / "romfs_overlay"
            os.environ.pop("DM_OVERLAY_MAC", None)
            os.environ.pop("DM_ASTC_PACK", None)
            os.environ["MOVIE_FFMPEG"] = "0"
            os.environ["DM_GEN_ASSETS"] = str(gen)
            os.environ["DM_HUD_V4_DIR"] = str(gen / "NativeHUD/v4")
            sys.path.insert(0, str(tools))
            for name in MODULES:
                sys.modules.pop(name, None)
            sys.modules["gen_assets"] = types.SimpleNamespace(
                ensure=lambda _, overlay_only=False: None
            )
            sys.modules["gen_merc_art"] = types.SimpleNamespace(
                ART=str(gen / "NativeHUD/mercslots"),
                # 固定的 gen_merc_art.py（0d65f6f3...b7b179），merc_slots.rects 也以它在生成图像上定位槽位。
                # Pinned gen_merc_art.py (0d65f6f3...b7b179), also used by
                # merc_slots.rects to place slots over the generated image.
                DX=63,
                DY=11,
            )

            def read(path):
                key = logical(
                    path if str(path).lower().startswith("data/") else "data/" + str(path)
                )
                require(key in assets, "OVERLAY_MISSING_AUTHENTICATED_ASSET:" + key)
                return assets[key]

            sys.modules["romfs_art"] = types.SimpleNamespace(read=lambda _, p: read(p))
            touch = importlib.import_module("touch_layout")

            def entry(_, row):
                data = read(logical(row[9]))
                require(hashlib.md5(data).hexdigest() == row[4], "OVERLAY_CKEY_MISMATCH")
                return data

            def layout(_, rel, suffix=None):
                want = (suffix or "/ui/layouts/" + rel).lower()
                row = next((f for f in rows if f[9].lower().endswith(want)), None)
                require(row is not None, "OVERLAY_MISSING_LAYOUT:" + want)
                return row[9].split(":", 1)[-1], entry(None, row).decode("utf-8-sig")

            touch._read_entry = entry
            touch.read_layout = layout
            Image.MAX_IMAGE_PIXELS = 4_194_304

            def capture(replacements, dst, tag, strings):
                nonlocal output_size
                dst = Path(dst)
                dst.mkdir(parents=True, exist_ok=True)
                name = (
                    "base"
                    if dst == out
                    else dst.relative_to(root / "romfs_overlay_variants").as_posix()
                )
                require(
                    name in ("base", "item_notes") and len(variants) < 2,
                    "OVERLAY_VARIANT",
                )
                inventory = []
                known = {logical(f[9]) for f in rows}
                require(len(replacements) <= 4096, "OVERLAY_REPLACEMENT_COUNT")
                for raw_path, source in replacements.items():
                    path = logical(raw_path.removeprefix("+"))
                    source = Path(source)
                    if not source.is_absolute():
                        source = Path(namespace["t"]) / source
                    require(
                        source.is_relative_to(root)
                        and source.is_file()
                        and source.stat().st_size <= 16 * 1024 * 1024,
                        "OVERLAY_REPLACEMENT_FILE",
                    )
                    data = source.read_bytes()
                    output_size += len(data)
                    require(output_size <= MAX_OUTPUT, "OVERLAY_OUTPUT_LIMIT")
                    key = f"file-{len(files)}"
                    files[key] = data
                    inventory.append({"path": path, "file": key, "add": path not in known})
                # 精确的原生附属文件表达式，这些文件不进入 CASC。
                # Exact native sidecar expression; these files do not enter CASC.
                (dst / "touch_strings.tsv").write_text(
                    "".join(
                        "%s\t%s\t%s\n"
                        % (
                            k,
                            o,
                            v.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t"),
                        )
                        for k, o, v in strings
                    )
                )
                (dst / "ctrl_touch").write_text(
                    "触屏版布局在 layouts/xtouch_lay、ztouch_console_lay、0touch_console_switch_lay（dm_padmode.c 见到本文件才改写前缀）\n"
                )
                (dst / "xbox_glyphs").write_text("ztouch_controller_buttons_ui/switchx*.sprite\n")
                (dst / "ps_glyphs").write_text("ztouch_controller_buttons_ui/switchp*.sprite\n")
                variants.append({"name": name, "replacements": inventory, "sidecars": []})
                return (
                    "browser-casc-pending",
                    0,
                )  # 仅用于内部原生日志，绝不是打包文件名。 | Internal native logging only, never a packaged filename.

            namespace = {
                "__name__": "__overlay_recipe__",
                "__file__": str(tools / "gen_romfs_overlay.py"),
                "__browser_capture": capture,
                "__browser_temp_root": str(root),
            }
            sys.argv = [str(tools / "gen_romfs_overlay.py"), str(romfs), str(out)]
            with (
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                exec(compile(RECIPE, namespace["__file__"], "exec"), namespace)
            require(
                [v["name"] for v in variants] == ["base", "item_notes"],
                "OVERLAY_VARIANT_SET",
            )
            for variant in variants:
                directory = (
                    out if variant["name"] == "base" else root / "romfs_overlay_variants/item_notes"
                )
                for p in sorted(directory.iterdir()):
                    require(
                        p.is_file()
                        and p.name
                        in (
                            "ctrl_touch",
                            "touch_strings.tsv",
                            "features.txt",
                            "dm_textbox_plain.tsv",
                            "xbox_glyphs",
                            "ps_glyphs",
                        ),
                        "OVERLAY_SIDECAR",
                    )
                    data = p.read_bytes()
                    output_size += len(data)
                    require(output_size <= MAX_OUTPUT, "OVERLAY_OUTPUT_LIMIT")
                    key = f"file-{len(files)}"
                    files[key] = data
                    variant["sidecars"].append({"path": p.name, "file": key})
            return {"files": files, "variants": variants}
    finally:
        builtins.open, io.open = old_open, old_io_open
        Image.MAX_IMAGE_PIXELS = old_pixels
        sys.path[:] = old_path
        sys.argv[:] = old_argv
        os.environ.clear()
        os.environ.update(old_env)
        for name, module in old_modules.items():
            sys.modules.pop(name, None)
            if module is not None:
                sys.modules[name] = module
