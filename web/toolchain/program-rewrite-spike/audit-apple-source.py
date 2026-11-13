"""对已审阅的仅 Apple 上游重写特化进行静态证明。只解析源码，不导入/执行 patch_nso.py，不加载原生/WASM 绑定。基线绑定整个 v2 前 AST；比较时唯一排除的输出新增项是经过明确审阅的 fixups.tsv 中 TARGET0 行。
Static proof of the reviewed Apple-only upstream rewriter specialization.

Parse source only. Do not import/execute patch_nso.py or load native/WASM bindings.
The baseline binds the entire pre-v2 AST. The only output addition excluded from
that comparison is the explicitly reviewed TARGET0 line in fixups.tsv.
"""

import ast
import copy
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[2] / "dm/tools/patch_nso.py"
BASELINE = HERE / "apple-source-baseline.json"
HELPERS = {
    "MRS_TPIDR",
    "emit_tp",
    "STR_PRE",
    "LDR_POST",
    "stash",
    "unstash",
    "uses_sp",
    "sp_frame",
}
ADDED_GLOBALS = {"TARGET", "OHOS_TSD_OFF", "NOREDZONE", "SPOP"}


def canonical(node):
    if isinstance(node, ast.AST):
        # Python 3.12 新增空 type_params，不影响这些函数。
        # Python 3.12 added empty type_params; it does not affect these functions.
        return {
            "node": type(node).__name__,
            **{
                key: canonical(value)
                for key, value in ast.iter_fields(node)
                if key != "type_params" or value
            },
        }
    if isinstance(node, bytes):
        return {"bytes": node.hex()}
    if isinstance(node, list):
        return [canonical(item) for item in node]
    return node


def digest(node):
    raw = json.dumps(
        canonical(node), sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def added_key(node):
    if isinstance(node, ast.FunctionDef) and node.name in HELPERS:
        return "function:" + node.name
    if (
        isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
    ):
        if node.targets[0].id in ADDED_GLOBALS:
            return "global:" + node.targets[0].id
    if isinstance(node, ast.Assert) and isinstance(node.test, ast.Compare):
        if isinstance(node.test.left, ast.Name) and node.test.left.id == "TARGET":
            return "assert:TARGET"
    return None


class Apple(ast.NodeTransformer):
    """只折叠已审阅且无副作用的 Apple 分支及包装器。
    Fold only the reviewed, side-effect-free Apple branches and wrappers."""

    function = None
    reordered = None

    def visit_FunctionDef(self, node):
        previous = self.function
        self.function = node.name
        node = self.generic_visit(node)
        if node.name == "site_tpidr_write":
            # 上游在首次 emit 前计算这个纯临时值；pick({N},1) 返回 0..15，因此 16/17 中总有一个可用。赋值及 emit 都不读写对方状态。
            # Upstream computes this pure temporary before the first emit.
            # pick({N},1) returns0..15; therefore one of16/17 always exists.
            # Neither the assignment nor emit reads/modifies the other's state.
            if (
                ast.unparse(node.body[3])
                != "tmp = next((x for x in (16, 17) if x not in (N, T)))"
                or ast.unparse(node.body[4]) != "emit(4162781184 | 31 << 5 | T)"
            ):
                raise ValueError("Unexpected Apple temporary motion")
            self.reordered = digest(node.body[3:5])
            node.body[3], node.body[4] = node.body[4], node.body[3]
        self.function = previous
        return node

    def visit_Name(self, node):
        constants = {"NOREDZONE": False}
        if self.function == "site_plain":
            constants["fr"] = None
        if self.function == "site_window":
            constants["nrz"] = False
        if isinstance(node.ctx, ast.Load) and node.id in constants:
            return ast.Constant(value=constants[node.id])
        return node

    def visit_BoolOp(self, node):
        # 仅折叠开头的 false 操作数，其余操作数永不执行。
        # Only fold a leading false operand: the remaining operands never run.
        node = self.generic_visit(node)
        if (
            isinstance(node.op, ast.And)
            and isinstance(node.values[0], ast.Constant)
            and not node.values[0].value
        ):
            return node.values[0]
        return node

    def visit_IfExp(self, node):
        node = self.generic_visit(node)
        if isinstance(node.test, ast.Constant):
            return node.body if node.test.value else node.orelse
        return node

    def visit_If(self, node):
        node = self.generic_visit(node)
        if isinstance(node.test, ast.Constant):
            return node.body if node.test.value else node.orelse
        return node

    def visit_Assign(self, node):
        node = self.generic_visit(node)
        expected = {"site_plain": ("fr", None), "site_window": ("nrz", False)}.get(
            self.function
        )
        if (
            expected
            and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id == expected[0]
        ):
            if (
                not isinstance(node.value, ast.Constant)
                or node.value.value is not expected[1]
            ):
                raise ValueError("Apple local specialization changed")
            return None
        return node

    def visit_Call(self, node):
        node = self.generic_visit(node)
        if not isinstance(node.func, ast.Name):
            return node
        name = node.func.id
        if name not in {"emit_tp", "stash", "unstash"}:
            return node
        if node.keywords:
            raise ValueError("Unexpected Apple wrapper keyword arguments")
        if name == "emit_tp":
            if len(node.args) != 1:
                raise ValueError("Unexpected emit_tp arguments")
            instruction = ast.Call(
                func=ast.Name(id="MRS_TPIDRRO", ctx=ast.Load()),
                args=node.args,
                keywords=[],
            )
        else:
            if (
                len(node.args) not in (2, 3)
                or len(node.args) == 3
                and not (
                    isinstance(node.args[2], ast.Constant)
                    and node.args[2].value is False
                )
            ):
                raise ValueError("Unexpected Apple stack wrapper arguments")
            instruction = ast.Call(
                func=ast.Name(id="STP" if name == "stash" else "LDP", ctx=ast.Load()),
                args=[
                    *node.args[:2],
                    ast.Constant(value=31),
                    ast.UnaryOp(op=ast.USub(), operand=ast.Constant(value=16)),
                ],
                keywords=[],
            )
        return ast.Call(
            func=ast.Name(id="emit", ctx=ast.Load()), args=[instruction], keywords=[]
        )


def specialize(tree):
    tree = copy.deepcopy(tree)
    removed = {}
    retained = []
    for node in tree.body:
        key = added_key(node)
        if key:
            if key in removed:
                raise ValueError("Duplicate target helper")
            removed[key] = digest(node)
        else:
            retained.append(node)
    tree.body = retained
    # 仅移除首个元数据写入，不删除任意带 TARGET 的代码。
    # Remove exactly the first metadata write, not arbitrary TARGET-bearing code.
    matches = [
        node
        for node in tree.body
        if isinstance(node, ast.With)
        and ast.unparse(node.items[0].context_expr) == "open(P('fixups.tsv'), 'w')"
    ]
    if len(matches) != 1 or not matches[0].body:
        raise ValueError("Missing fixups writer")
    removed["output:TARGET"] = digest(matches[0].body.pop(0))
    visitor = Apple()
    normalized = visitor.visit(tree)
    removed["motion:site_tpidr_write.tmp"] = visitor.reordered
    return normalized, removed


def verify(source=SOURCE, baseline=BASELINE):
    contract = json.loads(Path(baseline).read_text(encoding="utf-8"))
    data = Path(source).read_bytes()
    if hashlib.sha256(data).hexdigest() != contract["sourceSha256"]:
        raise ValueError("Apple rewriter source digest mismatch")
    normalized, removed = specialize(ast.parse(data.decode("utf-8")))
    if removed != contract["reviewedTargetNodes"]:
        raise ValueError("Reviewed target/helper/output AST changed")
    if digest(normalized) != contract["baselineAstSha256"]:
        raise ValueError("Apple-specialized AST differs from pre-v2 source")
    return contract


if __name__ == "__main__":
    result = verify()
    print(
        "Apple-only AST matches pre-v2 script; explicit TARGET0 metadata is the sole output-format addition; no source executed"
    )
