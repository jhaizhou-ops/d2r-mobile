#!/usr/bin/env python3
"""d2rm_pack.patched_ready: the rewritten program counts as done only when its recorded rewriter fingerprint is this patch_nso.py.
Expected values come from the files written here, not from the implementation."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import d2rm_pack  # noqa: E402

SRC = hashlib.sha256((TOOLS / "patch_nso.py").read_bytes()).hexdigest()


def products(d, fp):
    for m in ("main", "sdk"):
        (d / (m + ".patched.bin")).write_bytes(b"x")
        (d / (m + ".gates.tsv")).write_text("x")
    if fp is not None:
        (d / "build_src.sha256").write_text(fp + "  patch_nso.py\n")


class PatchedReadyTest(unittest.TestCase):
    def test_current_rewriter(self):
        with tempfile.TemporaryDirectory() as d:
            products(Path(d), SRC)
            self.assertTrue(d2rm_pack.patched_ready(d))

    def test_older_rewriter_same_product_code(self):
        with tempfile.TemporaryDirectory() as d:
            products(Path(d), "0" * 64)
            self.assertFalse(d2rm_pack.patched_ready(d))

    def test_no_fingerprint(self):
        with tempfile.TemporaryDirectory() as d:
            products(Path(d), None)
            self.assertFalse(d2rm_pack.patched_ready(d))

    def test_missing_gates(self):
        with tempfile.TemporaryDirectory() as d:
            products(Path(d), SRC)
            (Path(d) / "sdk.gates.tsv").unlink()
            self.assertFalse(d2rm_pack.patched_ready(d))


if __name__ == "__main__":
    unittest.main()
