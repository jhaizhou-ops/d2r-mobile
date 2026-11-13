"""仅合成原生外部引用对照。 | Synthetic-only native external-reference oracle."""

import contextlib
import io
import runpy
import tempfile
from pathlib import Path
import json
import base64
import hashlib

with contextlib.redirect_stdout(io.StringIO()):
    ns = runpy.run_path(str(Path(__file__).with_name("fixture.py")))
native = ns["native"]
files, vfs, root, keys = ns["namespace"]["fixture"]()
raw = b"external texture synthetic" * 30
encoded = native.encode_by_espec(raw, "z")
key = native.ekey_for(encoded, 0, 8, 0)
pack = {
    "name": "00-00000008.data",
    "size": len(encoded),
    "sha256": hashlib.sha256(encoded).hexdigest(),
}
records = [
    {
        "path": "data/z.bin",
        "oldEkey": keys[1][0].hex(),
        "newEkey": key.hex(),
        "encodedSize": len(encoded),
        "decodedSize": len(raw),
    }
]
replacement = b"ordinary replacement"
added = b"ordinary addition"
parts = [files["data/.build.config"], vfs, root, replacement, added]
jobs = [
    {"path": "data/n.bin", "oldEkey": keys[0][0].hex()},
    {"path": "data/x/new.bin", "add": True},
]
with tempfile.TemporaryDirectory() as temp:
    source = Path(temp) / "source"
    for name, data in files.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    repl = Path(temp) / "replacement"
    repl.write_bytes(replacement)
    add = Path(temp) / "addition"
    add.write_bytes(added)
    output = Path(temp) / "output"
    with contextlib.redirect_stdout(io.StringIO()):
        native.make_overlay(
            source,
            output,
            {"data/n.bin": repl, "+data/x/new.bin": add},
            {"data/z.bin": (key, len(encoded), len(raw))},
        )
    expected = {
        f"data/{name}": base64.b64encode((output / "data" / name).read_bytes()).decode()
        for name in (".build.config", "00-00000002.data")
    }
print(
    json.dumps(
        {
            "parts": [base64.b64encode(p).decode() for p in parts],
            "jobs": jobs,
            "external": {"pack": pack, "records": records},
            "expected": expected,
        }
    )
)
