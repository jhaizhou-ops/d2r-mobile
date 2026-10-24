"""打包已审核、固定来源的资产配方；每个子命令保留独立的 AST 边界与输出。
Package reviewed source-pinned asset recipes; each command preserves its AST boundary and outputs.
"""

import argparse
import ast
import base64
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[2]


def package_core():
    """仅打包已审计的纯函数，绝不导入或执行原生命令行模块。
    Package only audited pure functions, never import/execute native CLI modules."""
    SOURCES = {
        "dm/ios/tools/romfs_art.py": [],
        "dm/ios/tools/gen_hud_v4.py": ["dflt"],
        "dm/ios/tools/gen_hud_exp.py": ["main"],
        "dm/ios/tools/gen_romfs_overlay.py": ["touch_path"],
        "dm/ios/tools/shell_stubs.py": ["unq", "paths", "parse", "data"],
    }
    manifest = json.loads((HERE / "source-provenance.json").read_text())
    parts = ["import csv, io, os, re, struct, sys\nimport numpy as np\nfrom PIL import Image\n"]
    for path, names in SOURCES.items():
        raw = (ROOT / path).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if digest != manifest["sources"][path]:
            raise SystemExit(
                f"Native source drift: {path}. Review boundary before updating source-provenance.json."
            )
        tree = ast.parse(raw)
        for name in names:
            matches = [
                n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == name
            ]
            if len(matches) != 1:
                raise SystemExit(f"Expected one {name} in {path}")
            node = matches[0]
            if name == "main":
                node.name = "native_experience_main"
            parts.append(ast.unparse(node))
        if path.endswith("gen_romfs_overlay.py"):
            for name in ("LAY", "UI", "TDIR"):
                matches = [
                    n
                    for n in ast.walk(tree)
                    if isinstance(n, ast.Assign)
                    and any(isinstance(t, ast.Name) and t.id == name for t in n.targets)
                ]
                if len(matches) != 1:
                    raise SystemExit(f"Expected one {name}")
                ast.literal_eval(
                    matches[0].value
                )  # 若将来的赋值增加副作用则失败。 | Fail if a future assignment grows side effects.
                parts.append(ast.unparse(matches[0]))
        if path.endswith("shell_stubs.py"):
            constant = next(
                n
                for n in tree.body
                if isinstance(n, ast.Assign)
                and any(isinstance(t, ast.Name) and t.id == "CSTR" for t in n.targets)
            )
            ast.literal_eval(constant.value)
            parts.append(ast.unparse(constant))
    output = HERE / "public/generated"
    output.mkdir(parents=True, exist_ok=True)
    (output / "native_functions.py").write_text("\n\n".join(parts) + "\n")
    (output / "core.py").write_text((HERE / "src/core.py").read_text())
    (output / "derived.py").write_text((HERE / "src/derived.py").read_text())
    (output / "provenance.json").write_text(json.dumps(manifest, indent=2) + "\n")


def package_native_art():
    """打包未修改的原生美术生成器及已审计的非代码元数据。
    Package the unchanged native-art generator and its audited non-code metadata."""
    PATHS = {
        "generator": "dm/ios/tools/native_art/generate.py",
        "implementation": "dm/ios/tools/native_art/implementation.c.inc",
        "manifest": "dm/ios/Sources/TouchArt/native_art_manifest.json",
        "geometry": "dm/ios/docs/native_art_v1_geometry.json",
        "binding": "dm/ios/docs/touch_layout_spec_native_binding_v5.json",
    }
    pins = json.loads((HERE / "native-art-provenance.json").read_text())
    content = {}
    for name, path in PATHS.items():
        raw = (ROOT / path).read_bytes()
        if hashlib.sha256(raw).hexdigest() != pins[path]:
            raise SystemExit("Native-art source drift: " + path)
        content[name] = raw.decode()
    parts = ["import csv, hashlib, io, json\nfrom pathlib import Path\n"]
    allowed = {"sha", "canonical", "classes", "codes", "classes_by_code", "COMMON_ICON"}
    for node in ast.parse(content.pop("generator")).body:
        if isinstance(node, ast.FunctionDef) and node.name in {"main", "validate_rgba"}:
            parts.append(ast.unparse(node))
        elif isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in allowed for t in node.targets
        ):
            parts.append(ast.unparse(node))
    out = HERE / "public/generated"
    out.mkdir(parents=True, exist_ok=True)
    (out / "native_art_original.py").write_text("\n\n".join(parts) + "\n")
    (out / "native_art.py").write_text((HERE / "src/native_art.py").read_text())
    (out / "native-art-metadata.json").write_text(json.dumps(content, ensure_ascii=False))

    (out / "native-art-metadata-sha256.json").write_text(
        json.dumps(
            {k: hashlib.sha256(v.encode()).hexdigest() for k, v in content.items()},
            indent=2,
        )
        + "\n"
    )


def package_hud():
    """打包固定的原始 HUD 计算，不打包命令行设置或文件系统发现。
    Package pinned original HUD computations, not CLI setup or filesystem discovery."""
    paths = json.loads((HERE / "hud-provenance.json").read_text())
    source = {}
    for path, expected in paths.items():
        raw = (ROOT / path).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise SystemExit("HUD source drift: " + path)
        source[path] = raw.decode()
    out = HERE / "public/generated"
    out.mkdir(parents=True, exist_ok=True)
    tree = ast.parse(source["dm/ios/tools/gen_hud_v4.py"])
    # 命令行/环境设置在 load() 前结束，计算部分保持不变。
    # CLI/environment setup ends immediately before load(). Computations remain unchanged.
    start = next(
        i for i, n in enumerate(tree.body) if isinstance(n, ast.FunctionDef) and n.name == "load"
    )
    body = []
    for node in tree.body[start:]:
        if isinstance(node, ast.If) and ast.unparse(node.test) == "args.tuning_json is not None":
            continue  # 浏览器发布流程不接受任意开发者调参。 | No arbitrary developer tuning in the browser release path.
        if isinstance(node, ast.If) and ast.unparse(node.test) == "not args.overlay_only":
            body.extend(
                node.body
            )  # 浏览器 iOS 配方始终包含技能剪影。 | The browser iOS recipe always includes skill silhouettes.
            continue
        if isinstance(node, ast.Import) and any(n.name == "dm_i18n" for n in node.names):
            continue  # 提供隔离且固定来源的模块，不搜索文件系统导入。 | Supplied isolated source-pinned module, no filesystem import discovery.
        body.append(node)
    if any(
        isinstance(n, ast.Name) and n.id in {"args", "sys"} for node in body for n in ast.walk(node)
    ):
        raise SystemExit("Unexpected CLI dependency in packaged HUD recipe")
    wrapper = ast.parse("def generate(ROMFS, OUT, HERE, SRC):\n    pass\n").body[0]
    wrapper.body = body
    (out / "hud_original.py").write_text(
        "import json, re, os\nimport numpy as np\nfrom PIL import Image\n"
        + ast.unparse(ast.fix_missing_locations(wrapper))
        + "\n"
    )
    design = ast.parse(source["dm/ios/tools/hud_v4_design.py"])
    keep = [
        n
        for n in design.body
        if isinstance(n, ast.FunctionDef)
        and n.name == "load"
        or isinstance(n, ast.Assign)
        and any(isinstance(t, ast.Name) and t.id == "ART" for t in n.targets)
    ]
    (out / "hud_design_original.py").write_text(
        "import numpy as np\nfrom PIL import Image, ImageDraw, ImageFont, ImageFilter\n"
        + "\n\n".join(ast.unparse(n) for n in keep)
        + "\n"
    )
    parts = ["import csv, json, re\n"]
    for path, functions, constants in [
        ("dm/ios/tools/layout_patch.py", {"apply", "load_text"}, set()),
        (
            "dm/ios/tools/dm_i18n.py",
            {"rows"},
            {"PROPER", "FMT", "EN_LIMITS", "EN_HAS_ZH"},
        ),
    ]:
        for n in ast.parse(source[path]).body:
            if (
                isinstance(n, ast.FunctionDef)
                and n.name in functions
                or isinstance(n, ast.Assign)
                and any(isinstance(t, ast.Name) and t.id in constants for t in n.targets)
            ):
                parts.append(ast.unparse(n))
    (out / "hud_support_original.py").write_text("\n\n".join(parts) + "\n")
    (out / "hud.py").write_text((HERE / "src/hud.py").read_text())
    metadata = {
        "patch": source["dm/ios/tools/touchrectv1_patch.json"],
        "i18n": source["dm/i18n/ui_text.tsv"],
        "manifest": source["dm/ios/Sources/TouchArt/native_art_manifest.json"],
    }
    (out / "hud-metadata.json").write_text(json.dumps(metadata, ensure_ascii=False))
    (out / "hud-metadata-sha256.json").write_text(
        json.dumps({k: hashlib.sha256(v.encode()).hexdigest() for k, v in metadata.items()})
    )


def package_map_goal():
    """提取固定的地图目标渲染，显式区分旧版/上游清单分支。
    Extract pinned map-goal rendering with explicit legacy/upstream inventory branches."""
    pins = json.loads((HERE / "packager-source-pins.json").read_text())["map-goal"]
    path = "dm/ios/tools/gen_map_goal_art.py"
    raw = (ROOT / path).read_bytes()
    if hashlib.sha256(raw).hexdigest() != pins[path]:
        raise SystemExit("Map-goal source drift")
    tree = ast.parse(raw)
    # 默认保留已验证的旧清单。固定源码的 v2 main 仅新增一处韩语赋值、保存及清单项，对这些变化显式设门槛。
    # Keep the proven legacy inventory by default. The source-pinned v2 main adds
    # exactly one Korean assignment, save, and manifest entry; gate those explicitly.
    main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "main")
    main.args.args.append(ast.arg(arg="_include_korean"))
    main.args.defaults.append(ast.Constant(value=False))

    class KoreanBranch(ast.NodeTransformer):
        changes = 0

        def visit_Assign(self, node):
            if any(isinstance(t, ast.Name) and t.id == "ko" for t in node.targets):
                self.changes += 1
                return ast.If(
                    test=ast.Name(id="_include_korean", ctx=ast.Load()),
                    body=[node],
                    orelse=[],
                )
            return self.generic_visit(node)

        def visit_Expr(self, node):
            text = ast.unparse(node)
            if text.startswith("label(ko,"):
                self.changes += 1
                return ast.If(
                    test=ast.Name(id="_include_korean", ctx=ast.Load()),
                    body=[node],
                    orelse=[],
                )
            return self.generic_visit(node)

        def visit_Dict(self, node):
            for i, key in enumerate(node.keys):
                if isinstance(key, ast.Constant) and key.value == "koKR":
                    self.changes += 1
                    value = node.values[i]
                    node.keys[i] = None
                    node.values[i] = ast.IfExp(
                        test=ast.Name(id="_include_korean", ctx=ast.Load()),
                        body=ast.Dict(keys=[key], values=[value]),
                        orelse=ast.Dict(keys=[], values=[]),
                    )
            return node

    branch = KoreanBranch()
    branch.visit(main)
    if branch.changes != 3:
        raise SystemExit("Map-goal Korean branch source drift")
    ast.fix_missing_locations(tree)
    keep = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef):
            keep.append(node)
        elif isinstance(node, ast.Assign):
            names = {
                n.id for target in node.targets for n in ast.walk(target) if isinstance(n, ast.Name)
            }
            if names & {"NPC", "OBJ", "LABELS", "TEXT_PX", "PAD_X", "PAD_Y", "BOX_A"}:
                keep.append(node)
    out = HERE / "public/generated"
    out.mkdir(parents=True, exist_ok=True)
    (out / "map_goal_original.py").write_text(
        "import csv, json, os, struct\nfrom PIL import Image, ImageDraw, ImageFont\n"
        + "\n\n".join(ast.unparse(n) for n in keep)
        + "\n"
    )
    (out / "map_goal.py").write_text((HERE / "src/map_goal.py").read_text())


def package_casc_overlay():
    """仅打包固定源码的纯 CASC 函数，不模拟原生文件系统。
    Package source-pinned pure CASC functions only; no native filesystem emulation."""
    path = "dm/tools/casc_overlay.py"
    raw = (ROOT / path).read_bytes()
    expected = json.loads((HERE / "packager-source-pins.json").read_text())["casc-overlay"][path]
    if hashlib.sha256(raw).hexdigest() != expected:
        raise SystemExit("CASC overlay source drift")
    names = {
        "die",
        "norm_path",
        "blte_encode",
        "replace_config",
        "path_entry",
        "walk_paths",
        "trie_bytes",
        "offset_field_size",
        "add_paths",
        "set_cft_sizes",
        "cft_position",
        "encode_by_espec",
        "ekey_for",
    }
    nodes = [n for n in ast.parse(raw).body if isinstance(n, ast.FunctionDef) and n.name in names]
    if {n.name for n in nodes} != names:
        raise SystemExit("Missing CASC pure function")
    out = HERE / "public/generated"
    out.mkdir(parents=True, exist_ok=True)
    # 使用精确源码片段，不重新实现编码器。
    # Exact source segments, not reimplemented encoders.
    (out / "casc_overlay_original.py").write_text(
        "import hashlib, struct, zlib\n\n"
        + "\n\n".join(ast.get_source_segment(raw.decode(), n) for n in nodes)
        + "\n"
    )
    (out / "casc_overlay.py").write_text((HERE / "src/casc_overlay.py").read_text())


def package_mercenary():
    """提取原始佣兵合成，排除编解码及文件系统 I/O。
    Extract original mercenary composition, excluding codec and filesystem I/O."""
    path = "dm/ios/tools/gen_merc_art.py"
    pins = json.loads((HERE / "packager-source-pins.json").read_text())["mercenary"]
    raw = (ROOT / path).read_bytes()
    if hashlib.sha256(raw).hexdigest() != pins[path]:
        raise SystemExit("Mercenary source drift")
    tree = ast.parse(raw)
    constants = []
    body = None
    for node in tree.body:
        if isinstance(node, ast.Assign) and not any(
            isinstance(n, ast.Call) for n in ast.walk(node)
        ):
            constants.append(node)
        if isinstance(node, ast.FunctionDef) and node.name == "main":
            body = []
            for statement in node.body:
                if isinstance(statement, ast.Assign) and any(
                    isinstance(t, ast.Name) and t.id == "o" for t in statement.targets
                ):
                    break
                body.append(statement)
            assert len(body) == 19, "Original composition structure drift"
            body.append(ast.Return(value=ast.Name(id="im", ctx=ast.Load())))
            node.body = body
            node.name = "compose"
            constants.append(node)
    assert body is not None
    out = HERE / "public/generated"
    out.mkdir(parents=True, exist_ok=True)
    (out / "mercenary_original.py").write_text(
        "import struct\n" + "\n\n".join(ast.unparse(n) for n in constants) + "\n"
    )
    (out / "mercenary.py").write_text((HERE / "src/mercenary.py").read_text())


def package_overlay_recipe():
    """打包固定的原生覆盖语义，仅替换文件系统/CASC 边界。
    Package pinned native overlay semantics; replace only filesystem/CASC boundaries."""
    pins = json.loads((HERE / "overlay-recipe-provenance.json").read_text())
    sources = {}
    for path, expected in pins.items():
        raw = (ROOT / path).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise SystemExit(f"Overlay source drift: {path}")
        sources[path] = base64.b64encode(raw).decode()
    script = ast.parse((ROOT / "dm/ios/tools/gen_romfs_overlay.py").read_text())
    # 清单在真实 CASC 字节产生后由浏览器边界封装；捕获器不能替假归档签清单。
    # Finalize manifests after real CASC bytes at the browser boundary, never over captured placeholders.
    footer = next(
        i
        for i, node in enumerate(script.body)
        if isinstance(node, ast.Import)
        and any(name.name == "overlay_contract" for name in node.names)
    )
    if len(script.body[footer:]) != 5 or not isinstance(script.body[footer + 3], ast.For):
        raise SystemExit("Overlay manifest finalization boundary drift")
    script.body = script.body[:footer]
    emit = next(n for n in ast.walk(script) if isinstance(n, ast.FunctionDef) and n.name == "emit")
    # 保留原始 to_touch、继承重写及两项精确布局集合门槛。
    # Keep original to_touch, inheritance rewrites and both exact layout-set gates.
    cut = next(
        i
        for i, n in enumerate(emit.body)
        if isinstance(n, ast.With) and "repl.tsv" in ast.unparse(n)
    )
    emit.body = emit.body[:cut] + ast.parse("return __browser_capture(repl, dst, tag, TS)").body
    temporary_calls = [
        n
        for n in ast.walk(script)
        if isinstance(n, ast.Call) and ast.unparse(n.func) == "tempfile.TemporaryDirectory"
    ]
    if len(temporary_calls) != 1 or temporary_calls[0].args or temporary_calls[0].keywords:
        raise SystemExit("Overlay temporary-directory boundary drift")
    temporary_calls[0].keywords = [
        ast.keyword(arg="dir", value=ast.Name(id="__browser_temp_root", ctx=ast.Load()))
    ]
    ast.fix_missing_locations(script)
    out = HERE / "public/generated"
    out.mkdir(parents=True, exist_ok=True)
    (out / "overlay_recipe_sources.py").write_text(
        "SOURCES = " + repr(sources) + "\nRECIPE = " + repr(ast.unparse(script)) + "\n"
    )
    (out / "overlay_recipe.py").write_text((HERE / "src/overlay_recipe.py").read_text())


def package_remaining_art():
    """打包固定的原始仓库/宝箱/储藏库/袋子图像语句，不包含 I/O。
    Package pinned original stash/trove/vault/pouch image statements, without I/O."""
    PINS = json.loads((HERE / "remaining-art-provenance.json").read_text())
    for path, digest in PINS.items():
        if hashlib.sha256((ROOT / path).read_bytes()).hexdigest() != digest:
            raise SystemExit("Remaining art source drift: " + path)

    def nodes(name):
        return ast.parse((ROOT / "dm/ios/tools" / name).read_text()).body

    def constants(tree):
        return [
            node
            for node in tree
            if isinstance(node, ast.Assign)
            and not any(isinstance(child, ast.Call) for child in ast.walk(node))
        ]

    def composition(filename, start, stop, result, rename):
        tree = nodes(filename)
        main = next(n for n in tree if isinstance(n, ast.FunctionDef) and n.name == "main")
        body = main.body[start:stop]
        main.name = rename
        main.args = ast.arguments(
            posonlyargs=[],
            args=[ast.arg(arg="im")],
            kwonlyargs=[],
            kw_defaults=[],
            defaults=[],
        )
        main.body = body + [ast.Return(value=ast.Name(id=result, ctx=ast.Load()))]
        return constants(tree) + [main]

    wide = nodes("gen_widestash_art.py")
    wide = (
        constants(wide)
        + [
            n
            for n in wide
            if isinstance(n, ast.Assign)
            and any(isinstance(t, ast.Name) and t.id == "SPEC" for t in n.targets)
        ]
        + [n for n in wide if isinstance(n, ast.FunctionDef) and n.name == "widen_bg"]
    )
    trove = [n for n in nodes("gen_trove_art.py") if isinstance(n, (ast.Assign, ast.FunctionDef))]
    # 语句切片由源码哈希固定，并由下方无 I/O 的 AST 检查断言。
    # Statement slices are source-hash pinned, and asserted by the I/O-free AST check below.
    vault = composition("gen_vault_cover.py", 5, -3, "crop", "vault_cover")
    pouch = composition("gen_pouch_art.py", 4, -3, "out", "pouch_box")
    for tree in [wide, trove, vault, pouch]:
        ast.fix_missing_locations(ast.Module(body=tree, type_ignores=[]))
        for node in ast.walk(ast.Module(body=tree, type_ignores=[])):
            if isinstance(node, ast.Name) and node.id in {
                "open",
                "G",
                "dmx",
                "sys",
                "os",
            }:
                raise SystemExit("Unexpected I/O or codec dependency in extracted composition")
    out = HERE / "public/generated"
    out.mkdir(parents=True, exist_ok=True)
    for name, tree in [
        ("remaining_art_original", wide + vault + pouch),
        ("trove_original", trove),
    ]:
        (out / (name + ".py")).write_text(
            "import numpy as np\nfrom PIL import Image, ImageDraw\n"
            + "\n\n".join(ast.unparse(n) for n in tree)
            + "\n"
        )
    (out / "remaining_art.py").write_text((HERE / "src/remaining_art.py").read_text())


def package_vault():
    """从固定源码提取原始储藏库表函数及字面常量。
    Extract original vault table functions and literal constants from pinned source."""
    path = "dm/ios/tools/gen_vault_table.py"
    raw = (ROOT / path).read_bytes()
    if (
        hashlib.sha256(raw).hexdigest()
        != json.loads((HERE / "packager-source-pins.json").read_text())["vault"][path]
    ):
        raise SystemExit("Vault source drift")
    keep = []
    for node in ast.parse(raw).body:
        if isinstance(node, ast.FunctionDef):
            keep.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in {"PARTS", "PART_OF"} for t in node.targets
        ):
            ast.literal_eval(node.value)
            keep.append(node)
    out = HERE / "public/generated"
    out.mkdir(parents=True, exist_ok=True)
    (out / "vault_original.py").write_text(
        "import json, os, re\n" + "\n\n".join(ast.unparse(n) for n in keep) + "\n"
    )
    (out / "vault.py").write_text((HERE / "src/vault.py").read_text())


COMMANDS = {
    "core": package_core,
    "native-art": package_native_art,
    "hud": package_hud,
    "map-goal": package_map_goal,
    "casc-overlay": package_casc_overlay,
    "mercenary": package_mercenary,
    "overlay-recipe": package_overlay_recipe,
    "remaining-art": package_remaining_art,
    "vault": package_vault,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=[*COMMANDS, "all"])
    args = parser.parse_args()
    if args.command == "all":
        for package in COMMANDS.values():
            package()
    else:
        COMMANDS[args.command]()


if __name__ == "__main__":
    main()
