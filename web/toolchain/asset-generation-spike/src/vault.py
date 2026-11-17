"""有界内存原始储藏库生成，不接收游戏路径或可执行输入。
Bounded in-memory original vault generation; no game paths or executable inputs."""

import base64
import contextlib
import hashlib
import io
import json
import types

import vault_original as original
from derived import BoundedOutput

PATHS = tuple(
    "data/global/excel/" + n + ".txt"
    for n in (
        "ItemTypes",
        "armor",
        "weapons",
        "misc",
        "UniqueItems",
        "Sets",
        "SetItems",
    )
) + tuple("data/hd/items/" + n + ".json" for n in ("uniques", "sets", "items"))


def require(condition, code):
    if not condition:
        raise ValueError(code)


def generate(payload, options):
    require(len(payload) <= 20 * 1024 * 1024 and options == {}, "VAULT_INPUT_LIMIT")
    bundle = json.loads(payload.decode("utf-8"))
    require(
        isinstance(bundle, dict) and set(bundle) == {"assets", "manifest"},
        "VAULT_INPUT_SET",
    )
    require(
        isinstance(bundle["assets"], dict) and set(bundle["assets"]) == set(PATHS),
        "VAULT_ASSET_SET",
    )
    manifest = bundle["manifest"]
    require(
        isinstance(manifest, str) and len(manifest.encode()) <= 4 * 1024 * 1024,
        "VAULT_MANIFEST_LIMIT",
    )
    assets = {}
    for name in PATHS:
        raw = base64.b64decode(bundle["assets"][name], validate=True)
        require(0 < len(raw) <= 2 * 1024 * 1024, "VAULT_ASSET_LIMIT")
        if name.endswith(".txt"):
            require(len(raw.splitlines()) <= 100001, "VAULT_ROW_LIMIT")
        else:
            rows = json.loads(raw.decode("utf-8"))
            require(isinstance(rows, list) and len(rows) <= 100000, "VAULT_JSON_ROWS")
        assets[name] = raw
    require(sum(map(len, assets.values())) <= 12 * 1024 * 1024, "VAULT_TOTAL_LIMIT")
    # 原始存在性查找只关心条目的清单路径。
    # Only the item's manifest path matters to the original existence lookup.
    for line in manifest.splitlines():
        fields = line.split("|")
        require(
            len(fields) == 11
            and fields[9].startswith("data:data/hd/global/ui/items/")
            and fields[9].endswith(".sprite"),
            "VAULT_MANIFEST_ROW",
        )
    original.dmx = types.SimpleNamespace(get=lambda path: assets[path])
    original.sys = types.SimpleNamespace(argv=["gen_vault_table.py", "/input"])

    def memory_open(path, **_kwargs):
        require(path == "/input/data/build_manifest.psv", "VAULT_IO_BOUNDARY")
        return io.StringIO(manifest)

    original.open = memory_open
    output = BoundedOutput(limit=2 * 1024 * 1024)
    try:
        with contextlib.redirect_stdout(output):
            original.main()
        result = bytes(output.buffer)
        return {
            "files": {"dm_vault_tab.h": result},
            "sourceSha256": {name: hashlib.sha256(raw).hexdigest() for name, raw in assets.items()},
            "manifestSha256": hashlib.sha256(manifest.encode()).hexdigest(),
        }
    except SystemExit as exc:
        raise ValueError("VAULT_NATIVE_REJECTED") from exc
    finally:
        del original.dmx, original.sys, original.open
