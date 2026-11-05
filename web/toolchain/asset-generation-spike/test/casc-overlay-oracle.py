"""合成的原始原生参考，不使用受版权保护的游戏或密钥输入。仅使用 Python 标准库运行，输出供 JS 边界测试使用的 JSON 样例。
Synthetic original-native oracle, no copyrighted game or key input.
Run with Python stdlib only; output JSON fixture consumed by the JS boundary test.
"""

import base64
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE / "public/generated"))
bounded = importlib.import_module("casc_overlay")

spec = importlib.util.spec_from_file_location(
    "native_overlay_oracle", ROOT / "dm/tools/casc_overlay.py"
)
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


def tvfs(keys, specs, paths=True):
    strings = b"\0".join(s.encode() for s in specs) + b"\0"
    cft = b"".join(
        k + struct.pack(">I", enc) + bytes([idx]) + struct.pack(">I", dec)
        for k, enc, idx, dec in keys
    )
    width = native.offset_field_size(len(cft))
    vfs = b"".join(
        b"\x01" + struct.pack(">II", 0, dec) + (i * 25).to_bytes(width, "big")
        for i, (_, _, _, dec) in enumerate(keys)
    )
    tree, _ = native.trie_bytes(
        {
            name: i * (9 + width)
            for i, name in enumerate([b"n.bin", b"z.bin", b"b.bin"][: len(keys)])
        }
    )
    tree = (
        b"\x04data\x00\xff" + struct.pack(">I", 0x80000000 | len(tree) + 4) + tree
        if paths
        else tree
    )
    header = bytearray(b"TVFS\x01\x2e\x10\x00" + struct.pack(">I", 10) + b"\0" * 34)
    est = 46 + len(tree)
    coff = est + len(strings)
    voff = coff + len(cft)
    struct.pack_into(">6I", header, 12, 46, len(tree), voff, len(vfs), coff, len(cft))
    struct.pack_into(">H2I", header, 36, 2, est, len(strings))
    return bytes(header) + tree + strings + cft + vfs


def fixture():
    archive = bytearray()

    def append(raw, mode):
        enc = native.encode_by_espec(raw, mode)
        key = native.ekey_for(enc, 0, 1, len(archive))
        archive.extend(enc)
        return key, len(enc)

    old = [b"old raw", b"old zlib", b"old blte"]
    keys = [append(raw, mode) for raw, mode in zip(old, ("n", "z", "b:256K*=z"))]
    vfs = tvfs(
        [(key, size, i, len(raw)) for i, ((key, size), raw) in enumerate(zip(keys, old))],
        ["n", "z", "b:256K*=z"],
    )
    vk, ve = append(vfs, "b:256K*=z")
    root = tvfs([(vk, ve, 0, len(vfs))], ["b:256K*=z"], False)
    rk, re = append(root, "b:256K*=z")
    config = (
        "# synthetic only\n"
        + f"vfs-1 = {hashlib.md5(vfs).hexdigest()} {vk.hex()}\nvfs-1-size = {len(vfs)} {ve}\n"
        + "".join(
            f"{name} = {hashlib.md5(root).hexdigest()} {rk.hex()}\n{name}-size = {len(root)} {re}\n"
            for name in ("vfs-root", "vfs-2")
        )
    ).encode()
    rows = []
    for name, (key, size), content, mode in zip(
        ("n.bin", "z.bin", "b.bin"), keys, old, ("n", "z", "b:256K*=z")
    ):
        rows.append(
            "|".join(
                [
                    "0",
                    "0",
                    "0",
                    "1",
                    hashlib.md5(content).hexdigest(),
                    str(len(content)),
                    key.hex(),
                    str(size),
                    mode,
                    "data:" + "data/" + name,
                    "0",
                ]
            )
        )
    manifest = ("# synthetic header\n" + "\n".join(rows) + "\n").encode()
    return (
        {
            "data/.build.config": config,
            "data/build_manifest.psv": manifest,
            "data/00-00000001.data": bytes(archive),
        },
        vfs,
        root,
        keys,
    )


def run():
    files, vfs, root, keys = fixture()
    replacements = [
        ("data/n.bin", b""),
        ("data/z.bin", b"zlib replacement" * 100),
        ("data/b.bin", bytes(range(256)) * 1100),
        ("+data/x/new.bin", b"added original trie"),
    ]
    parts = [files["data/.build.config"], vfs, root] + [b for _, b in replacements]
    jobs = [
        {
            "path": p.lstrip("+"),
            "add": p.startswith("+"),
            **({"oldEkey": keys[i][0].hex()} if i < 3 else {}),
        }
        for i, (p, _) in enumerate(replacements)
    ]
    options = {"parts": [len(x) for x in parts], "jobs": jobs, "location": [0, 2]}
    result = bounded.generate(b"".join(parts), options)
    with tempfile.TemporaryDirectory() as tmp:
        temp = Path(tmp)
        source = temp / "source"
        source.mkdir()
        (source / "data").mkdir()
        for path, raw in files.items():
            (source / path).write_bytes(raw)
        reps = {}
        for i, (path, raw) in enumerate(replacements):
            file = temp / f"replacement-{i}"
            file.write_bytes(raw)
            reps[path] = file
        with contextlib.redirect_stdout(io.StringIO()):
            native.make_overlay(source, temp / "out", reps)
        for path, raw in result["files"].items():
            assert (temp / "out" / path).read_bytes() == raw, path
    # 检查 CKey/配置不一致、尾部载荷、重复任务及恶意大小。
    # CKey/config inconsistency, trailing payload, duplicate job, malicious sizes.
    for payload, opt in [
        (b"".join(parts) + b"x", options),
        (b"".join(parts), {**options, "location": [0, 256]}),
        (b"".join(parts), {**options, "parts": [-1] + options["parts"][1:]}),
    ]:
        try:
            bounded.generate(payload, opt)
        except (ValueError, SystemExit):
            pass
        else:
            raise AssertionError("invalid request accepted")

    def encode(obj):
        return {k: base64.b64encode(v).decode() for k, v in obj.items()}

    print(
        json.dumps(
            {
                "files": encode(files),
                "replacements": [
                    {
                        "path": p.lstrip("+"),
                        "add": p.startswith("+"),
                        "bytes": base64.b64encode(raw).decode(),
                    }
                    for p, raw in replacements
                ],
                "expected": encode(result["files"]),
                "inventory": result["inventory"],
            }
        )
    )


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "serialize":
        request = json.load(sys.stdin)
        result = bounded.generate(base64.b64decode(request["payload"]), request["options"])
        result["files"] = {k: base64.b64encode(v).decode() for k, v in result["files"].items()}
        print(json.dumps(result))
    else:
        run()
