#!/usr/bin/env python3
"""仅供测试构建：在可丢弃源码副本中替换一个零售验证模数。生产构建从不调用，也不引入运行时根覆盖。
Test build only: replace one retail verification modulus in a disposable source copy.
Never called by the production build; no runtime root override is introduced.
"""

from pathlib import Path
import re
import sys

root, modulus_file = map(Path, sys.argv[1:])
p = root / "pki.c"
s = p.read_text()
pattern = r"(static const nca_keyset_t nca_keys_retail = \{\s*\.nca_hdr_fixed_key_moduli = \{ /\* Fixed RSA key used to validate NCA signature 0\. \*/\s*\{)([^}]+)(\})"
matches = list(re.finditer(pattern, s))
if len(matches) != 1:
    raise SystemExit("Pinned upstream retail-root source layout mismatch")
m = matches[0]
old = re.findall(r"0x[0-9A-Fa-f]{2}", m.group(2))
if len(old) != 256 or bytes(int(x, 16) for x in old)[:8] != bytes.fromhex("BFBE406CF4A780E9"):
    raise SystemExit("Pinned upstream retail verification root mismatch")
modulus = modulus_file.read_bytes()
if len(modulus) != 256 or modulus[0] < 128 or not modulus[-1] & 1:
    raise SystemExit("Invalid synthetic RSA-2048 modulus")
replacement = (
    "\n"
    + "\n".join(
        "            " + ", ".join(f"0x{x:02X}" for x in modulus[i : i + 16]) + ","
        for i in range(0, 256, 16)
    )
    + "\n        "
)
p.write_text(s[: m.start(2)] + replacement + s[m.end(2) :])
