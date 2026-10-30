"""仅解析源码数据来生成原生覆盖层来源指纹，不执行原生生成器。
Generate the native overlay source fingerprint by parsing data, never executing generators.
"""

import argparse
import ast
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
OUTPUT = Path(__file__).resolve().parents[1] / "contract-source.json"
RELEASE = "b811b90172f4eb40ec1bc89d903308e0c5ab88d0"


def generate(release=False):
    def read(relative):
        if release:
            return subprocess.check_output(
                ["git", "show", RELEASE + ":" + relative], cwd=ROOT
            )
        return (ROOT / relative).read_bytes()

    tree = ast.parse(read("dm/ios/tools/overlay_contract.py").decode("utf-8-sig"))
    value = next(
        node.value
        for node in tree.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == "SOURCE_INPUTS"
            for target in node.targets
        )
    )
    if (
        ast.unparse(value.func) != "tuple"
        or ast.unparse(value.args[0].func) != "sorted"
    ):
        raise ValueError("Overlay source-input declaration changed")
    paths = sorted(ast.literal_eval(value.args[0].args[0]))
    digest = hashlib.sha256(b"DM_OVERLAY_SOURCE_V1\0")
    sources = {}
    for relative in paths:
        data = (
            read(relative)
            .decode("utf-8-sig")
            .replace("\r\n", "\n")
            .replace("\r", "\n")
            .encode()
        )
        if len(data) > 8 * 1024 * 1024:
            raise ValueError("Overlay source input too large: " + relative)
        digest.update(relative.encode("ascii") + b"\0")
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
        sources[relative] = hashlib.sha256(data).hexdigest()
    return {
        **({"sourceCommit": RELEASE} if release else {}),
        "sourceFingerprint": digest.hexdigest(),
        "normalizedSources": sources,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--release",
        action="store_true",
        help="Read exact b811 release git blobs, never current sources",
    )
    args = parser.parse_args()
    result = generate(args.release)
    output = (
        OUTPUT.with_name("release-contract-source.json") if args.release else OUTPUT
    )
    if args.check:
        if json.loads(output.read_text()) != result:
            raise SystemExit("Overlay contract source fingerprint is stale")
    else:
        output.write_text(json.dumps(result, indent=2) + "\n")
    print(
        "Verified overlay contract source fingerprint: " + result["sourceFingerprint"]
    )
