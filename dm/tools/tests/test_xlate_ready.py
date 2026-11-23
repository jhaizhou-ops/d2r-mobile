#!/usr/bin/env python3
"""#299: the packing tool's shader-translator step and its error message.
d2rm_pack.xlate_ready: with a prebuilt translator (the shell's, or DM_XLATE_PREBUILT), a different copy left in the work folder
(for example v2.3.1's framework-dependent one) does not count as done, and place_xlate replaces it. Without a prebuilt one, presence
is enough, as before. dmsh_chain.xlate_problem: a translator that did not run is reported as such, not as "not built from the
repository's source"."""
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS.parents[0] / "gfx" / "tools"))
import d2rm_pack  # noqa: E402
import dmsh_chain  # noqa: E402


def shell_zip(files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in files.items():
            z.writestr(name, data)
    buf.seek(0)
    return zipfile.ZipFile(buf)


class XlateReadyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.xl = os.path.join(self.tmp.name, "dmsh_xlate")
        self.pre = d2rm_pack.XLATE_IN_SHELL + d2rm_pack.plat() + "/"
        os.environ.pop("DM_XLATE_PREBUILT", None)

    def tearDown(self):
        self.tmp.cleanup()
        os.environ.pop("DM_XLATE_PREBUILT", None)

    def put(self, name, data):
        os.makedirs(self.xl, exist_ok=True)
        with open(os.path.join(self.xl, name), "wb") as f:
            f.write(data)

    def test_missing_is_not_ready(self):
        self.assertFalse(d2rm_pack.xlate_ready(self.xl, shell_zip({self.pre + "dmsh_xlate": b"aot"})))

    def test_old_shell_copy_is_replaced(self):
        # v2.3.1 left a framework-dependent translator; the new shell carries a NativeAOT single file
        self.put("dmsh_xlate", b"apphost needing .NET")
        self.put("dmsh_xlate.dll", b"managed code")
        z = shell_zip({self.pre + "dmsh_xlate": b"nativeaot single file"})
        self.assertFalse(d2rm_pack.xlate_ready(self.xl, z))
        self.assertTrue(d2rm_pack.place_xlate(self.xl, z))
        self.assertTrue(d2rm_pack.xlate_ready(self.xl, z))
        self.assertFalse(os.path.exists(os.path.join(self.xl, "dmsh_xlate.dll")))

    def test_same_copy_is_ready(self):
        self.put("dmsh_xlate", b"nativeaot single file")
        self.assertTrue(d2rm_pack.xlate_ready(self.xl, shell_zip({self.pre + "dmsh_xlate": b"nativeaot single file"})))

    def test_other_platform_in_shell_is_ignored(self):
        self.put("dmsh_xlate", b"built with dotnet here")
        z = shell_zip({d2rm_pack.XLATE_IN_SHELL + "some-other-platform/dmsh_xlate": b"aot"})
        self.assertTrue(d2rm_pack.xlate_ready(self.xl, z))

    def test_no_prebuilt_keeps_presence_check(self):
        self.put("dmsh_xlate.exe", b"built with dotnet here")
        self.assertTrue(d2rm_pack.xlate_ready(self.xl, None))

    def test_env_prebuilt_compared(self):
        src = os.path.join(self.tmp.name, "prebuilt")
        os.makedirs(src)
        with open(os.path.join(src, "dmsh_xlate"), "wb") as f:
            f.write(b"prebuilt")
        os.environ["DM_XLATE_PREBUILT"] = src
        self.put("dmsh_xlate", b"stale")
        self.assertFalse(d2rm_pack.xlate_ready(self.xl, None))
        self.put("dmsh_xlate", b"prebuilt")
        self.assertTrue(d2rm_pack.xlate_ready(self.xl, None))


class XlateProblemTest(unittest.TestCase):
    def test_not_running_is_reported_as_such(self):
        msg = dmsh_chain.xlate_problem("?退出码 131：You must install .NET to run this application.")
        self.assertIn("没跑起来", msg)
        self.assertIn("131", msg)
        self.assertNotIn("不是用仓内源编的", msg)

    def test_wrong_source_keeps_its_message(self):
        msg = dmsh_chain.xlate_problem("d760b2f173ee5e0c")
        self.assertIn("不是用仓内源编的", msg)
        self.assertIn("d760b2f173ee5e0c", msg)


if __name__ == "__main__":
    unittest.main()
