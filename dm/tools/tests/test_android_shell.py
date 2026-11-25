#!/usr/bin/env python3
"""#243：安卓导入包工具的 --shell。给了发布页同一版本的壳 zip，就用里面这个平台预编的着色器翻译工具，不再要求 .NET SDK；
壳的产物代号与源码不一致时停下；壳里没有这个平台的翻译工具时照旧要 .NET。不碰游戏文件。运行：python3 dm/tools/tests/test_android_shell.py
| #243: --shell in the Android pack tool. With the shell zip of the same release, this platform's prebuilt shader translator inside it
is used and the .NET SDK is no longer required; a shell whose product code differs from the source stops the tool; a shell without a
translator for this platform still needs .NET. No game files are needed. Run: python3 dm/tools/tests/test_android_shell.py"""
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import d2rm_android as tool  # noqa: E402
import d2rm_pack  # noqa: E402


def make_shell(path, product, with_xlate=True):
    """一个最小的壳 zip：Payload 里一个 App（manifest、shell_info），可选 Tools/dmsh_xlate/<本平台>/
    | A minimal shell zip: one App in Payload (manifest, shell_info), optionally Tools/dmsh_xlate/<this platform>/"""
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("Payload/DmIOS.app/DmIOS", b"exe")
        z.writestr("Payload/DmIOS.app/dm/shell_manifest.txt", "dm/shell_info.txt\n")
        z.writestr("Payload/DmIOS.app/dm/shell_info.txt", "PRODUCT=%s\nKIND=ios\n" % product)
        if with_xlate:
            exe = "dmsh_xlate.exe" if d2rm_pack.plat().startswith("win-") else "dmsh_xlate"
            z.writestr(d2rm_pack.XLATE_IN_SHELL + d2rm_pack.plat() + "/" + exe, b"prebuilt translator")


class AndroidShellTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.shell = os.path.join(self.tmp.name, "shell.zip")
        self.game = os.path.join(self.tmp.name, "game")
        for f in ("ExeFS/main", "ExeFS/sdk", "RomFS/data/build_manifest.psv"):
            p = os.path.join(self.game, f); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").close()
        self.saved = (os.environ.pop("DM_XLATE_PREBUILT", None), shutil.which)

    def tearDown(self):
        prebuilt, which = self.saved
        if prebuilt is not None:
            os.environ["DM_XLATE_PREBUILT"] = prebuilt
        shutil.which = which
        self.tmp.cleanup()

    def env_failure(self, z):
        """check_env 的报错文字（没有 dotnet 时）；全都齐了返回空串 | check_env's failure text (with no dotnet); empty when nothing is missing"""
        real = self.saved[1]
        shutil.which = lambda name, *a, **k: None if name == "dotnet" else real(name, *a, **k)
        home = os.environ.get("HOME"); os.environ["HOME"] = self.tmp.name
        try:
            tool.check_env(self.game, z)
            return ""
        except SystemExit as e:
            return str(e.code)
        finally:
            if home is not None: os.environ["HOME"] = home

    def test_mismatched_product_stops(self):
        make_shell(self.shell, "3.3.94012-r00")
        with self.assertRaises(SystemExit) as e:
            tool.open_shell_tools(self.shell, "3.3.94012-r45")
        self.assertIn("3.3.94012-r00", str(e.exception.code))

    def test_missing_shell_file_stops(self):
        with self.assertRaises(SystemExit):
            tool.open_shell_tools(os.path.join(self.tmp.name, "nope.zip"), "3.3.94012-r45")

    def test_matching_shell_with_translator_needs_no_dotnet(self):
        make_shell(self.shell, "3.3.94012-r45")
        z = tool.open_shell_tools(self.shell, "3.3.94012-r45")
        self.assertTrue(d2rm_pack.xlate_prebuilt(z))
        self.assertNotIn(".NET SDK", self.env_failure(z))

    def test_shell_without_translator_still_needs_dotnet(self):
        make_shell(self.shell, "3.3.94012-r45", with_xlate=False)
        z = tool.open_shell_tools(self.shell, "3.3.94012-r45")
        self.assertFalse(d2rm_pack.xlate_prebuilt(z))
        self.assertIn(".NET SDK", self.env_failure(z))

    def test_no_shell_needs_dotnet(self):
        self.assertIn(".NET SDK", self.env_failure(None))

    def test_prebuilt_env_needs_no_dotnet(self):
        os.environ["DM_XLATE_PREBUILT"] = self.tmp.name
        try:
            self.assertNotIn(".NET SDK", self.env_failure(None))
        finally:
            os.environ.pop("DM_XLATE_PREBUILT", None)

    def test_translator_is_placed_from_the_shell(self):
        make_shell(self.shell, "3.3.94012-r45")
        z = tool.open_shell_tools(self.shell, "3.3.94012-r45")
        xl = os.path.join(self.tmp.name, "dmsh_xlate")
        self.assertTrue(d2rm_pack.place_xlate(xl, z))
        self.assertTrue(d2rm_pack.xlate_ready(xl, z))


if __name__ == "__main__":
    unittest.main()
