"""原始函数的纯标准库对照；只处理合成输入。 | Original stdlib-only oracle on synthetic inputs."""

import ast
import importlib
import base64
import contextlib
import io
import json
from pathlib import Path
import sys
import types

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "web/toolchain/asset-generation-spike/src"


def functions(path, names=None, constants=()):
    tree = ast.parse((ROOT / path).read_text())
    nodes = [
        n
        for n in ast.walk(tree)
        if isinstance(n, ast.FunctionDef)
        and (names is None or n.name in names)
        or isinstance(n, ast.Assign)
        and any(isinstance(t, ast.Name) and t.id in constants for t in n.targets)
    ]
    namespace = dict(
        json=json,
        io=io,
        contextlib=contextlib,
        types=types,
        csv=__import__("csv"),
        re=__import__("re"),
        os=__import__("os"),
        sys=sys,
    )
    exec(compile(ast.Module(body=nodes, type_ignores=[]), path, "exec"), namespace)
    return namespace


native = types.ModuleType("native_functions")
for path, names, constants in [
    ("dm/ios/tools/gen_hud_exp.py", {"main"}, ()),
    ("dm/ios/tools/gen_romfs_overlay.py", {"touch_path"}, ("LAY", "UI", "TDIR")),
    ("dm/ios/tools/shell_stubs.py", {"unq", "paths", "parse", "data"}, ("CSTR",)),
]:
    values = functions(path, names, constants)
    for name in names | set(constants):
        setattr(native, "native_experience_main" if name == "main" else name, values[name])
    if "main" in names:
        experience_globals = values
    if "parse" in names:
        derived_globals = values
sys.modules["native_functions"] = native
original = types.ModuleType("vault_original")
original.__dict__.update(
    functions("dm/ios/tools/gen_vault_table.py", constants=("PARTS", "PART_OF"))
)
sys.modules["vault_original"] = original
sys.path.insert(0, str(SOURCE))
derived = importlib.import_module("derived").generate
vault = importlib.import_module("vault").generate

core = functions("web/toolchain/asset-generation-spike/src/core.py", {"execute"})["execute"]
core.__globals__["native"] = native

for line in sys.stdin:
    request = json.loads(line)
    payload = base64.b64decode(request["payload"])
    try:
        operation = request["operation"]
        if operation == "experience-table":
            experience_globals["romfs_art"] = types.SimpleNamespace(read=lambda *_: payload)
        if operation == "derived-tables":
            # 保留原始函数的共享 globals 字典，允许适配器替换 open。 | Keep original shared globals while the adapter installs its open boundary.
            native.parse = lambda *args: derived_globals["parse"](*args)
            original_data = derived_globals["data"]

            def data(*args):
                derived_globals["open"] = native.open
                return original_data(*args)

            native.data = data

            def parse(*args):
                derived_globals["open"] = native.open
                return derived_globals["parse"](*args)

            native.parse = parse
            result = derived(payload, request.get("options", {}))
        elif operation == "vault-table":
            # 函数使用模块的实际命名空间。 | Functions use the module's actual namespace.
            for value in list(original.__dict__.values()):
                if isinstance(value, types.FunctionType):
                    value.__globals__.update(original.__dict__)
            old_main = original.main

            def main():
                old_main.__globals__.update(original.__dict__)
                return old_main()

            original.main = main
            result = vault(payload, request.get("options", {}))
            original.main = old_main
        else:
            result = core(operation, payload, request.get("options", {}))
        result["files"] = {
            name: base64.b64encode(data).decode() for name, data in result["files"].items()
        }
        print(json.dumps({"ok": True, "result": result}))
    except Exception as error:
        print(json.dumps({"ok": False, "error": type(error).__name__ + ":" + str(error)}))
