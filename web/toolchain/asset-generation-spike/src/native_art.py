"""在有界、显式内存输入之后运行完整原始原生美术生成器。
Complete original native-art generator behind bounded, explicit memory inputs."""

import base64
import hashlib
import json
from pathlib import Path
import tempfile
import types

from PIL import Image
import native_art_original as original


def require(condition, code):
    if not condition:
        raise ValueError(code)


def generate(payload, options):
    require(len(payload) <= 20 * 1024 * 1024, "NATIVE_ART_INPUT_LIMIT")
    bundle = json.loads(payload.decode())
    metadata = bundle["metadata"]
    require(
        set(metadata) == {"manifest", "geometry", "binding", "implementation"},
        "NATIVE_ART_METADATA_SET",
    )
    pins = options.get("metadataSha256", {})
    require(set(pins) == set(metadata), "NATIVE_ART_TRUST_REQUIRED")
    for name, text in metadata.items():
        require(
            isinstance(text, str) and len(text.encode()) <= 2 * 1024 * 1024,
            "NATIVE_ART_METADATA_LIMIT",
        )
        require(
            hashlib.sha256(text.encode()).hexdigest() == pins[name],
            "NATIVE_ART_METADATA_HASH",
        )
    manifest = json.loads(metadata["manifest"])
    assets = manifest["assets"]
    require(9 <= len(assets) <= 64, "NATIVE_ART_ASSET_COUNT")
    names = [a["asset"] for a in assets]
    require(
        len(set(names)) == len(names)
        and all(
            n.startswith("dm_native_")
            and n.endswith(".png")
            and "/" not in n
            and "\\" not in n
            and len(n) < 128
            for n in names
        ),
        "NATIVE_ART_OUTPUT_NAME",
    )
    require(
        {t["file"] for t in manifest["official_tables"]} == {"skills.txt", "skilldesc.txt"}
        and len(manifest["official_tables"]) == 2,
        "NATIVE_ART_TABLE_SET",
    )
    paths = [a["official_path"].removeprefix("data:data/") for a in assets]
    paths += ["global/excel/" + t["file"] for t in manifest["official_tables"]]
    require(
        len(set(paths)) == len(paths) and set(bundle["raw"]) == set(paths),
        "NATIVE_ART_INPUT_SET",
    )
    require(set(bundle["rgba"]) == set(names), "NATIVE_ART_RGBA_SET")
    raw = {p: base64.b64decode(v, validate=True) for p, v in bundle["raw"].items()}
    require(sum(map(len, raw.values())) <= 8 * 1024 * 1024, "NATIVE_ART_RAW_LIMIT")
    images = {}
    pixels = 0
    for asset, path in zip(assets, paths):
        w, h = asset["atlas_px"]
        require(
            type(w) is int and type(h) is int and w > 0 and h > 0,
            "NATIVE_ART_DIMENSIONS",
        )
        pixels += w * h
        require(pixels <= 4_194_304, "NATIVE_ART_PIXEL_LIMIT")
        rgba = base64.b64decode(bundle["rgba"][asset["asset"]], validate=True)
        require(len(rgba) == w * h * 4, "NATIVE_ART_RGBA_SIZE")
        images[path] = Image.frombytes("RGBA", (w, h), rgba)
    with tempfile.TemporaryDirectory(prefix="native-art-") as tmp:
        root = Path(tmp)
        art = root / "Sources/TouchArt"
        art.mkdir(parents=True)
        (root / "docs").mkdir()
        (art / "native_art_manifest.json").write_text(metadata["manifest"])
        (root / "docs/native_art_v1_geometry.json").write_text(metadata["geometry"])
        (root / "docs/touch_layout_spec_native_binding_v5.json").write_text(metadata["binding"])
        (root / "implementation.c.inc").write_text(metadata["implementation"])
        original.IOS = root
        original.HERE = root
        original.romfs_art = types.SimpleNamespace(
            read=lambda _, p: raw[p], sprite=lambda _, p: images[p].copy()
        )
        try:
            original.main(None)
            files = {name: (art / name).read_bytes() for name in names}
            # 除 v2 的 RGBA 防护外，保留浏览器现有编码字节防护。
            # Keep the browser's existing encoded-byte guard in addition to v2's RGBA guard.
            for asset in assets:
                require(
                    hashlib.sha256(files[asset["asset"]]).hexdigest()
                    == asset["decoded_png_sha256"],
                    "NATIVE_ART_PNG_HASH",
                )
            files["dm_touch_native_art.c"] = (root / "Sources/dm_touch_native_art.c").read_bytes()
            require(
                sum(map(len, files.values())) <= 20 * 1024 * 1024,
                "NATIVE_ART_OUTPUT_LIMIT",
            )
            return {"files": files, "assetCount": len(assets)}
        except SystemExit as error:
            raise ValueError(str(error)) from error
        finally:
            del original.IOS, original.HERE, original.romfs_art
            for im in images.values():
                im.close()
