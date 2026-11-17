"""从固定 Python AST 提取/审计 JS 规则表，仅在构建时执行，不使用游戏输入。默认运行 audit-js-source.py 时拒绝源码或生成数据漂移；--write 重新生成后须应用仓库格式化器。不执行 patch_nso.py，不替换解码器，也不改变生产输入。
Extract/audit JS rule tables from the pinned Python AST. Build-time only, no game input.

    python3 audit-js-source.py          # fail closed on source or generated-data drift
    python3 audit-js-source.py --write  # regenerate, then apply the repository formatter

This does not execute patch_nso.py, substitute decoders, or alter production inputs.
"""

import argparse
import ast
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[2] / "dm/tools/patch_nso.py"
OUTPUT = HERE / "js/rule-data.mjs"


def named_call(node, name):
    return isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == name


def append_call(node, name):
    return (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "append"
        and isinstance(node.func.value, ast.Name)
        and node.func.value.id == name
    )


def extract():
    digest = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    expected = (HERE / "upstream-source.sha256").read_text().split()[0]
    if digest != expected:
        raise RuntimeError("Original patch_nso.py source pin mismatch")
    tree = ast.parse(SOURCE.read_text(encoding="utf-8"))
    tables = {"SOURCE_SHA256": digest}
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Assign)
            and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id in ("RULE_BUILD", "CUTS", "OBS", "PARAMS", "run_words")
        ):
            tables[node.targets[0].id.upper()] = ast.literal_eval(node.value)
    tables["PRIVATE_OBS"] = [
        ast.literal_eval(node.args[0]) for node in ast.walk(tree) if append_call(node, "obs")
    ]
    tables["RUN_REASONS"] = [
        ast.literal_eval(node.args[-1]) for node in ast.walk(tree) if named_call(node, "run_branch")
    ]
    branches = []
    for block in tree.body:
        if not isinstance(block, ast.If):
            continue
        assignment = next(
            (
                node
                for node in block.body
                if isinstance(node, ast.Assign)
                and isinstance(node.targets[0], ast.Tuple)
                and [getattr(item, "id", None) for item in node.targets[0].elts]
                in (["off", "word"], ["off", "word", "tgt"])
            ),
            None,
        )
        if assignment is None:
            continue
        gates = [node.args[0] for node in ast.walk(block) if append_call(node, "gates")]
        if not gates:
            continue
        assert len(gates) == 1
        values = list(ast.literal_eval(assignment.value))
        if len(values) == 2:
            # 原始视觉门控将目标写为字面 B 参数。
            # The original visual gate writes its target as a literal B argument.
            target = next(
                node.args[1]
                for node in ast.walk(block)
                if named_call(node, "B") and isinstance(node.args[1], ast.Constant)
            )
            values.append(ast.literal_eval(target))
        why = next(node.args[0].elts[3] for node in ast.walk(block) if append_call(node, "cuts"))
        kind = next(node.args[0] for node in ast.walk(block) if named_call(node, "stat"))
        branches.append(
            values
            + [
                ast.literal_eval(gates[0].elts[2]),
                ast.literal_eval(kind),
                ast.literal_eval(why),
            ]
        )
    tables["PRIVATE_BRANCHES"] = branches
    # 将 Python 元组规范化为 JavaScript 使用的 JSON 数组。
    # Canonicalize Python tuples to the JSON arrays used in JavaScript.
    return json.loads(json.dumps(tables, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="regenerate the JS literal tables")
    options = parser.parse_args()
    expected = extract()
    if options.write:
        header = "/** Literal rule data from dm/tools/patch_nso.py. Do not alter independently of the pinned source. */\n"
        output = [header]
        for name, value in expected.items():
            output.append(
                f"export const {name} = {json.dumps(value, ensure_ascii=False, indent=2)};"
            )
        OUTPUT.write_text("\n\n".join(output) + "\n", encoding="utf-8")
    actual = subprocess.run(
        [
            "node",
            "--input-type=module",
            "--eval",
            "import * as data from './js/rule-data.mjs'; process.stdout.write(JSON.stringify(data));",
        ],
        cwd=HERE,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if json.loads(actual.stdout) != expected:
        raise RuntimeError(
            "Generated JS rule data differs from the pinned Python AST; regenerate deliberately"
        )
    print(
        f"JS rule tables match pinned source {expected['SOURCE_SHA256']}; {len(expected['PRIVATE_BRANCHES'])} private branches, {len(expected['PRIVATE_OBS'])} private observations"
    )


if __name__ == "__main__":
    main()
