#!/usr/bin/env python3
"""除一个公开根外，两棵准备好的上游源码树有任何差异均失败。
Fail unless the two prepared upstream source trees differ only in one public root."""

from pathlib import Path
import sys
import tempfile
import subprocess

work = Path(sys.argv[1])


def source(name):
    found = list((work / name).glob("hactool.*"))
    if len(found) != 1:
        raise SystemExit("Expected exactly one pinned dependency source")
    return found[0]


prod, test = source("prod-build"), source("test-build")
# 比较所有被跟踪的上游源文件，而不只比较安全敏感文件。
# Compare all tracked upstream source files, rather than just security-sensitive ones.
tracked = subprocess.check_output(["git", "-C", str(prod), "ls-files", "-z"]).decode().split("\0")
for name in filter(None, tracked):
    if name == "pki.c":
        continue
    if (prod / name).read_bytes() != (test / name).read_bytes():
        raise SystemExit(f"Unexpected test dependency source change: {name}")
with tempfile.TemporaryDirectory(dir=work) as scratch:
    p = Path(scratch)
    (p / "pki.c").write_bytes((prod / "pki.c").read_bytes())
    subprocess.run(
        [
            sys.executable,
            str(Path(__file__).with_name("inject-test-root.py")),
            str(p),
            str(work / "fixtures/test-root.bin"),
        ],
        check=True,
    )
    if (p / "pki.c").read_bytes() != (test / "pki.c").read_bytes():
        raise SystemExit("Test root substitution exceeded its scope")
if (prod / "pki.c").read_bytes() == (test / "pki.c").read_bytes():
    raise SystemExit("Test root was not substituted")
print("Isolation verified: same pinned sources; only one test public root differs.")
