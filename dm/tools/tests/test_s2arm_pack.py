#!/usr/bin/env python3
"""d2rm_pack.s2arm_pack: the S2Arm resource pack (pushed by the S2Arm launcher, unpacked by ios/Sources/s2a_pack_apply.c).
The archive list must be the host's need[] list; index.tsv / manifest.json are checked against values computed here from the inputs."""
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import d2rm_pack  # noqa: E402


class S2ArmPackTest(unittest.TestCase):
    def test_archive_list_is_the_hosts(self):
        src = (TOOLS.parents[0] / "host" / "romfs_validate.c").read_text(encoding="utf-8")
        block = re.search(r"static const char \*need\[\] = \{(.*?)\};", src, re.S).group(1)
        self.assertEqual(d2rm_pack.PHONE_ARCHIVES, re.findall(r'"([^"]+)"', block))

    def test_pack_index_and_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            romfs = Path(d) / "romfs"; (romfs / "data").mkdir(parents=True)
            body = {n: (n.encode() * (i + 1)) for i, n in enumerate(d2rm_pack.PHONE_ARCHIVES)}
            for n, b in body.items():
                (romfs / "data" / n).write_bytes(b)
            (romfs / "data" / "99-00000001.data").write_bytes(b"not for the phone")
            out = Path(d) / "pack"
            n, total = d2rm_pack.s2arm_pack(str(romfs), str(out))
            want = "".join(f"data/{k}\t{len(v)}\t{hashlib.sha256(v).hexdigest()}\n" for k, v in body.items())
            self.assertEqual((n, total), (18, sum(map(len, body.values()))))
            self.assertEqual((out / "index.tsv").read_text(encoding="utf-8"), want)
            m = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(m, {"version": 2, "files": 18, "total": total, "index_sha256": hashlib.sha256(want.encode()).hexdigest()})
            self.assertEqual(sorted(p.name for p in (out / "data").iterdir()), sorted(body))
            for k, v in body.items():
                self.assertEqual((out / "data" / k).read_bytes(), v)


if __name__ == "__main__":
    unittest.main()
