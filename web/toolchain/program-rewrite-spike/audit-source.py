"""依据原始仓库源码验证浏览器发布常量，而非相信声明。
Verify browser release constants against original repository sources, not claims."""

import ast
import hashlib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
source = ROOT / "dm/tools/patch_nso.py"
text = source.read_text()
extract = (ROOT / "dm/tools/extract_game.sh").read_text()
pack = (ROOT / "dm/tools/d2rm_pack.py").read_text()
catalog = (ROOT / "web/program-release-catalog.mjs").read_text()
assignments = {}
for node in ast.parse(text).body:
    if (
        isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
    ):
        try:
            assignments[node.targets[0].id] = ast.literal_eval(node.value)
        except (ValueError, TypeError):
            pass
hashes = dict(
    (name, digest)
    for digest, name in re.findall(r"^([0-9a-f]{64}) (main|sdk|subsdk0)$", extract, re.M)
)
assert set(hashes) == {"main", "sdk", "subsdk0"}
for name in ("main", "sdk"):
    registered = re.search(
        r'name:\s*"' + name + r'".*?inputSha256:\s*"([0-9a-f]{64})"', catalog, re.S
    )
    assert registered and registered.group(1) == hashes[name], f"{name} full-file digest drift"
assert hashlib.sha256(source.read_bytes()).hexdigest() in catalog
build = assignments["RULE_BUILD"]
assert build in catalog
assert set(assignments["CUTS"]) == set(assignments["OBS"]) == set(assignments["PARAMS"]) == {build}
pack_assignments = {}
for node in ast.parse(pack).body:
    if isinstance(node, ast.Assign):
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id == "MODS":
                pack_assignments["MODS"] = ast.literal_eval(node.value)
assert pack_assignments["MODS"] == ["main", "sdk"]
assert "env={'DM_RULESET': 'touch-run-v4'}" in pack
# 这些是未改动源码所用的全部 Capstone 指令属性。
# These are all Capstone instruction attributes consumed by the unchanged source.
attributes = sorted(set(re.findall(r"\b(?:ins|dd|x|p|q)\.([a-z_]+)", text)))
assert set(attributes) <= {
    "bytes",
    "address",
    "mnemonic",
    "op_str",
    "operands",
    "reg_name",
    "regs_access",
}, attributes
report = {
    "sourceSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "registeredMainBuildId": build,
    "requiredModules": pack_assignments["MODS"],
    "ruleset": "touch-run-v4",
    "inputHashSource": "dm/tools/extract_game.sh:SUMS",
    "sdkBuildIdInCatalog": None,
    "fullSdkHashPublished": True,
    "realOutputHashesInCatalog": False,
    "capstoneAttributesUsed": attributes,
    "realGameVerified": False,
}
(HERE / "dist/source-audit.json").write_text(json.dumps(report, indent=2) + "\n")
print("Repository release input constants, original rule tables and binding surface verified")
