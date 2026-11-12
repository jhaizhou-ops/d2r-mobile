#!/usr/bin/env python3
"""The packing path may use only what the S2Arm launcher ships (docs/S2ARM.md section 3).
Starting from d2rm_pack.py and d2rm_android.py, follow every script they run (string literals naming a *.py in the repository)
and every repository module they import; each import must be the standard library, a repository module, or one of the
third-party packages S2Arm bundles (its launchers/macos/requirements-runtime.txt: capstone, lz4, numpy, Pillow)."""
import ast
from pathlib import Path
import re
import sys
import unittest

DM = Path(__file__).resolve().parents[2]
SEARCH = [DM / "tools", DM / "ios" / "tools", DM / "gfx" / "tools", DM / "tools" / "winlf", DM / "android" / "tools", DM / "verify"]
S2ARM_BUNDLED = {"PIL", "numpy", "lz4", "capstone"}   # S2Arm launchers/macos/requirements-runtime.txt
ROOTS = ["d2rm_pack.py", "d2rm_android.py"]


def find(name):
    for d in SEARCH:
        p = d / name
        if p.is_file():
            return p
    return None


def walk():
    """→ {script path: set of top-level imported names}, over everything reachable from ROOTS"""
    todo, seen = [find(r) for r in ROOTS], {}
    while todo:
        p = todo.pop()
        if p is None or p in seen:
            continue
        src = p.read_text(encoding="utf-8")
        names = set()
        for node in ast.walk(ast.parse(src)):
            if isinstance(node, ast.Import):
                names |= {a.name.split(".")[0] for a in node.names}
            elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
                names.add(node.module.split(".")[0])
        seen[p] = names
        for n in names:
            q = find(n + ".py")
            if q:
                todo.append(q)
        for lit in re.findall(r"['\"]([A-Za-z0-9_]+\.py)['\"]", src):
            q = find(lit)
            if q:
                todo.append(q)
    return seen


class S2ArmDepsTest(unittest.TestCase):
    def test_packing_path_uses_only_s2arm_bundled_packages(self):
        scripts = walk()
        self.assertGreater(len(scripts), 5, "the walk found too few scripts; the root names may have moved")
        bad = {}
        for p, names in scripts.items():
            for n in names:
                if n in sys.stdlib_module_names or n in S2ARM_BUNDLED or find(n + ".py") or n == "__future__":
                    continue
                bad.setdefault(n, []).append(str(p.relative_to(DM)))
        self.assertEqual(bad, {}, "imports S2Arm does not ship (docs/S2ARM.md section 3)")


if __name__ == "__main__":
    unittest.main()
