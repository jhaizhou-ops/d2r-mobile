"""仅生成指令/NSO 元数据，从不读取游戏输入。
Generated instructions/NSO metadata only. Never reads game inputs."""

import hashlib
import json
import os
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import lz4.block

HERE = Path(__file__).resolve().parent
# HERE 指向仓库的 web/toolchain/program-rewrite-spike。
# HERE = repository/web/toolchain/program-rewrite-spike
SOURCE = HERE.parents[2] / "dm/tools/patch_nso.py"
OUT = HERE / "fixtures"
OUT.mkdir(exist_ok=True)


def nso(words, compressed=False, registered=False):
    text = bytearray(struct.pack("<I", 0xD503201F) * 64)
    struct.pack_into("<II", text, 0, 0x14000010, 0x100)
    for i, word in enumerate(words):
        struct.pack_into("<I", text, 0x40 + 4 * i, word)
    ro = bytearray(0x100)
    ro[:4] = b"MOD0"
    struct.pack_into("<i", ro, 16, 0x20)
    # 异常帧头：版本 1，绝对 u32 指针/计数/表，单个函数。
    # eh_frame_hdr: version 1, absolute u32 pointer/count/table, one function.
    struct.pack_into("<4BIIII", ro, 0x20, 1, 3, 3, 3, 0, 1, 0x40, 0)
    segments = [bytes(text), bytes(ro), bytes(0x100)]
    payloads = [lz4.block.compress(s, store_size=False) if compressed else s for s in segments]
    header = bytearray(0x100)
    header[:4] = b"NSO0"
    struct.pack_into("<I", header, 0xC, 7 if compressed else 0)
    header[0x40:0x50] = (
        bytes.fromhex("db0507f36b84200ac05f75d76ed5026f")
        if registered
        else bytes.fromhex("534e5448455449430000000000000000")
    )
    offset = 0x100
    for i, payload in enumerate(payloads):
        struct.pack_into("<III", header, 0x10 + 0x10 * i, offset, i * 0x100, 0x100)
        struct.pack_into("<I", header, 0x60 + i * 4, len(payload))
        offset += len(payload)
    return bytes(header) + b"".join(payloads)


# 真实 ARM64 编码：SVC、TPIDR 读写、TPIDRRO、物理/虚拟计数器、x18 加法、加载/存储、cbz、adr 及 ret。不复制游戏指令或数据。
# Real ARM64 encodings: SVC, TPIDR read/write, TPIDRRO, physical/virtual counters,
# x18 add, load/store, cbz, adr, and ret. No game instructions/data are copied.
cases = [
    (
        "gates",
        [
            0xD4000541,
            0xD53BD040,
            0xD51BD041,
            0xD53BD062,
            0xD53BE023,
            0xD53BE044,
            0xD65F03C0,
        ],
        False,
        False,
        "native",
        True,
    ),
    (
        "x18",
        [
            0x91000652,
            0xF9000012,
            0xF9400012,
            0xB4000052,
            0x91000652,
            0x10000012,
            0xD65F03C0,
        ],
        False,
        False,
        "native",
        True,
    ),
    ("compressed", [0x91000652, 0xD4000541, 0xD65F03C0], True, False, "native", True),
    ("wrong-build-rule", [0xD65F03C0], False, False, "touch-run-v4", False),
    ("cut-preimage-mismatch", [0xD65F03C0], False, True, "native", False),
]
manifest = {
    "sourceSha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
    "cases": [],
}
catalog = {
    "schemaVersion": 1,
    "scope": "synthetic-only",
    "sourceSha256": manifest["sourceSha256"],
    "versions": [],
}
for name, words, compressed, registered, ruleset, success in cases:
    data = nso(words, compressed, registered)
    path = OUT / f"{name}.nso"
    path.write_bytes(data)
    target = HERE / "oracle" / name
    result = subprocess.run(
        [sys.executable, str(SOURCE), str(path), str(target), "main"],
        env={
            **os.environ,
            "DM_TARGET": "apple",
            "DM_RULESET": ruleset,
            "DM_PRIVATE_GATES": "1",
        },
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    expected_error = {
        "wrong-build-rule": "Run规则构建号不符",
        "cut-preimage-mismatch": "剪口原字节不符",
    }.get(name)
    if expected_error and expected_error not in result.stderr.decode():
        raise RuntimeError(f"{name}: expected upstream assertion was not reached")
    if (result.returncode == 0) != success:
        raise RuntimeError(f"{name}: unexpected native result: {result.stderr.decode()}")
    if success:
        guest_dir = HERE / "oracle-guest" / name
        subprocess.run(
            [
                sys.executable,
                str(SOURCE.with_name("guest_dylib.py")),
                str(target),
                str(guest_dir),
                "main",
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        framework = guest_dir / "Guest.framework"
        catalog["versions"].append(
            {
                "id": name,
                "buildId": data[0x40:0x50].hex(),
                "ruleset": ruleset,
                "inputSha256": hashlib.sha256(data).hexdigest(),
                "files": {
                    file.name: hashlib.sha256(file.read_bytes()).hexdigest()
                    for file in sorted(target.iterdir())
                },
                "guestSha256": hashlib.sha256((framework / "Guest").read_bytes()).hexdigest(),
                "plistSha256": hashlib.sha256((framework / "Info.plist").read_bytes()).hexdigest(),
            }
        )
    manifest["cases"].append(
        {
            "name": name,
            "sha256": hashlib.sha256(data).hexdigest(),
            "ruleset": ruleset,
            "success": success,
            "errorIncludes": expected_error,
        }
    )
# 独立 SDK 样例以 NAME=sdk 重写，然后原生 Guest 按精确原生打包顺序合并两个模块，不转换源码。
# A separate SDK fixture is rewritten with NAME=sdk, then native Guest combines
# both modules in the exact native packager order. No source transformation.

sdk_data = nso([0xD53BD040, 0x91000652, 0xD4000561, 0xD65F03C0], compressed=True)
sdk_path = OUT / "sdk-gates.nso"
sdk_path.write_bytes(sdk_data)
multi_target = HERE / "oracle" / "main-sdk"
multi_target.mkdir(parents=True, exist_ok=True)
for file in (HERE / "oracle" / "gates").iterdir():
    shutil.copyfile(file, multi_target / file.name)
subprocess.run(
    [sys.executable, str(SOURCE), str(sdk_path), str(multi_target), "sdk"],
    env={
        **os.environ,
        "DM_TARGET": "apple",
        "DM_RULESET": "native",
        "DM_PRIVATE_GATES": "1",
    },
    check=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
multi_guest = HERE / "oracle-guest" / "main-sdk"
subprocess.run(
    [
        sys.executable,
        str(SOURCE.with_name("guest_dylib.py")),
        str(multi_target),
        str(multi_guest),
        "main",
        "sdk",
    ],
    check=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
manifest["cases"].append(
    {
        "name": "sdk-gates",
        "moduleName": "sdk",
        "sha256": hashlib.sha256(sdk_data).hexdigest(),
        "ruleset": "native",
        "success": True,
        "multiOnly": True,
    }
)
main_recipe = catalog["versions"][0]
main_recipe = {
    key: value
    for key, value in main_recipe.items()
    if key not in ("guestSha256", "plistSha256", "id")
}
main_recipe.update({"name": "main", "caseName": "gates"})
sdk_recipe = {
    "name": "sdk",
    "caseName": "sdk-gates",
    "buildId": sdk_data[0x40:0x50].hex(),
    "ruleset": "native",
    "inputSha256": hashlib.sha256(sdk_data).hexdigest(),
    "files": {
        file.name: hashlib.sha256(file.read_bytes()).hexdigest()
        for file in sorted(multi_target.iterdir())
        if file.name.startswith("sdk.") or file.name == "build_src.sha256"
    },
}
multi_catalog = {
    "schemaVersion": 2,
    "scope": "synthetic-only",
    "sourceSha256": manifest["sourceSha256"],
    "versions": [
        {
            "id": "main-sdk",
            "modules": [main_recipe, sdk_recipe],
            "guestSha256": hashlib.sha256(
                (multi_guest / "Guest.framework/Guest").read_bytes()
            ).hexdigest(),
            "plistSha256": hashlib.sha256(
                (multi_guest / "Guest.framework/Info.plist").read_bytes()
            ).hexdigest(),
        }
    ],
}
(OUT / "trusted-multi-catalog.json").write_text(json.dumps(multi_catalog, indent=2) + "\n")
(OUT / "trusted-synthetic-catalog.json").write_text(json.dumps(catalog, indent=2) + "\n")
(OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"Generated {len(manifest['cases'])} synthetic cases and independent native oracle outputs")
