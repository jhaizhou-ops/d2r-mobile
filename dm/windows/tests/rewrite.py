# 从真实改写器抽取无资源编码器，生成可在 ARM64 实机执行的合成指令；不导入或伪造游戏映像。
# Extract resource-independent encoders from the actual rewriter and generate synthetic instructions for native ARM64 execution; no game image is loaded or fabricated.
import argparse
import ast
from itertools import combinations
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

DM = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(DM / "windows" / "tools"))
from asm_coff import convert
from check_arm64 import check


def encoders(target="windows"):
    tree = ast.parse((DM / "tools" / "patch_nso.py").read_text(encoding="utf-8"))
    constants = {"KEY_INDEX", "OHOS_TSD_OFF", "SPOP", "NOP"}
    nodes = [node for node in tree.body if isinstance(node, ast.FunctionDef) or
             isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in constants for t in node.targets)]
    namespace = dict(struct=struct, re=re, combinations=combinations, TARGET=target,
                     NOREDZONE=target in ("android", "windows"), PATCH_OFF=0x1000, KEY_PAGE_OFF=0x4000,
                     patch=bytearray(), fix=[], gates=[], stats={})
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "patch_nso.py encoders", "exec"), namespace)
    return namespace


def instruction(word, operands="", mnemonic="mrs"):
    return SimpleNamespace(bytes=struct.pack("<I", word), address=0x100, op_str=operands, mnemonic=mnemonic)


def emit_blocks(path):
    blocks = [
        ("read_tls", "site_sysreg_read", 0xD53BD040, "KTLS"),
        ("read_tlr", "site_sysreg_read", 0xD53BD060, "KTLR"),
        ("read_x18", "site_sysreg_read", 0xD53BD052, "KTLS"),
        ("read_zero", "site_sysreg_read", 0xD53BD05F, "KTLS"),
        ("write_tls", "site_tpidr_write", 0xD51BD040, None),
        ("write_x18", "site_tpidr_write", 0xD51BD052, None),
    ]
    output = ["// 自造指令，不含游戏内容。 | Synthetic instructions, no game content."]
    for name, function, word, kind in blocks:
        ns = encoders()
        args = (instruction(word), kind) if kind else (instruction(word),)
        ns[function](*args)
        words = struct.unpack(f"<{len(ns['patch']) // 4}I", ns["patch"])
        output.append(f"static const uint32_t rewrite_{name}[] = {{" + ",".join(f"0x{w:08x}u" for w in words) + "};")
    path.write_text("\n".join(output) + "\n", encoding="utf-8")


class ToolsTest(unittest.TestCase):
    def test_target_opcodes(self):
        expected = {"windows": [0xAA1203E7], "apple": [0xD53BD067],
                    "android": [0xD53BD047, 0xF85F80E7],
                    "ohos": [0xD53BD047, 0xD10440E7, 0xF94000E7]}
        for target, words in expected.items():
            ns = encoders(target)
            ns["emit_tp"](7)
            self.assertEqual(bytes(ns["patch"]), struct.pack(f"<{len(words)}I", *words))

    def test_allocated_stash(self):
        ns = encoders()
        ns["stash"](0, 1)
        ns["unstash"](0, 1)
        self.assertEqual(struct.unpack("<II", ns["patch"]), (0xA9BF07E0, 0xA8C107E0))
        ins = instruction(0xF8626BF2, "x18, [sp, x2]", "ldr")
        self.assertIsNone(ns["sp_frame"](ins, 0xF8626BE0, 0))
        with self.assertRaisesRegex(RuntimeError, "exclusive window uses sp"):
            ns["site_window"]([ins])

    def test_coff_symbols_and_literals(self):
        text = ('    .globl _fn\n_fn:\n    adrp x0, _data@PAGE\n'
                '    add x0, x0, _data@PAGEOFF\n    .private_extern _fn\n'
                '    .cstring\n    .asciz "_leave_this_string"\n')
        converted = convert(text)
        self.assertIn(".globl fn\nfn:", converted)
        self.assertIn("adrp x0, data", converted)
        self.assertIn("add x0, x0, :lo12:data", converted)
        self.assertIn('.section .rdata,"dr"', converted)
        self.assertIn('"_leave_this_string"', converted)
        self.assertNotIn(".private_extern", converted)
        entry = convert((DM / "host" / "entry.S").read_text(encoding="utf-8"), True)
        self.assertIn("bl dm_windows_gate_dispatch", entry)
        self.assertNotIn(".globl dm_thread_enter", entry)
        with self.assertRaises(ValueError):
            convert("missing stack bridge", True)

    def test_pe_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.dll"
            for machine in (0xAA64, 0x8664, 0xA641):
                data = bytearray(128)
                data[:2] = b"MZ"
                struct.pack_into("<I", data, 0x3C, 64)
                data[64:68] = b"PE\0\0"
                struct.pack_into("<H", data, 68, machine)
                path.write_bytes(data)
                if machine == 0xAA64:
                    check(path)
                else:
                    with self.assertRaises(ValueError):
                        check(path)

    @unittest.skipUnless(os.name == "nt", "Windows launcher environment")
    def test_launcher_environment(self):
        powershell = shutil.which("pwsh") or shutil.which("powershell")
        self.assertIsNotNone(powershell, "Windows PowerShell is required")
        script = r"""
param([string]$Launcher, [string]$Root)
$ErrorActionPreference = 'Stop'
$ast = [Management.Automation.Language.Parser]::ParseFile($Launcher, [ref]$null, [ref]$null)
$start = $ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.AssignmentStatementAst] -and
    $_.Left -is [Management.Automation.Language.VariableExpressionAst] -and
    $_.Left.VariablePath.UserPath -eq 'settings'
} | Select-Object -First 1
if (-not $start) { throw 'Launcher environment scope not found' }
$launch = [scriptblock]::Create($ast.Extent.Text.Substring($start.Extent.StartOffset))
$romfs = Join-Path $Root 'romfs'
$overlay = Join-Path $Root 'overlay'
$shaders = Join-Path $Root 'shaders'
$SaveDir = Join-Path $Root 'saves'
$out = Join-Path $Root 'out'
$Executable = 'Test-Child'
$names = @('DM_ROOT', 'DM_ROMFS', 'DM_ROMFS_OVERLAY', 'DM_VK_SHADER_DIR', 'DM_SAVE',
    'DM_TEMP', 'DM_APP', 'DM_WINDOW', 'DM_WINDOWS_GPU_BATCHING', 'DM_PRESENT_HZ', 'DM_PRESENT_INTERVAL')
function Test-Child {
    if (-not [object]::Equals($ExpectedHz, [Environment]::GetEnvironmentVariable('DM_PRESENT_HZ', 'Process')) -or
        -not [object]::Equals($ExpectedInterval, [Environment]::GetEnvironmentVariable('DM_PRESENT_INTERVAL', 'Process'))) {
        throw 'Child received incorrect pacing'
    }
    $global:LASTEXITCODE = $ChildExitCode
}
foreach ($initial in @([NullString]::Value, '', 'inherited')) {
    foreach ($target in @($null, 60, 120)) {
        foreach ($ExperimentalGpuBatching in @($false, $true)) {
            foreach ($ChildExitCode in @(0, 7)) {
                $before = @{}
                foreach ($name in $names) {
                    [Environment]::SetEnvironmentVariable($name, $initial, 'Process')
                    $before[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
                }
                [Nullable[int]]$TargetFps = $target
                $ExpectedHz = if ($null -ne $TargetFps) { "$TargetFps" } else { $before.DM_PRESENT_HZ }
                $ExpectedInterval = if ($null -ne $TargetFps) { 'display' } else { $before.DM_PRESENT_INTERVAL }
                $failed = $false
                try { & $launch } catch {
                    if ($ChildExitCode -ne 7 -or $_.Exception.Message -ne 'Windows host exited with code 7.') { throw }
                    $failed = $true
                }
                if ($failed -ne ($ChildExitCode -ne 0)) { throw 'Child failure was not reported' }
                foreach ($name in $names) {
                    if (-not [object]::Equals($before[$name], [Environment]::GetEnvironmentVariable($name, 'Process'))) {
                        throw "Environment restoration mismatch: $name"
                    }
                }
            }
        }
    }
}
foreach ($target in @(0, 30, 121, -1)) {
    $rejected = $false
    try { & $Launcher -Root $Root -GameDir $Root -TargetFps $target -CheckOnly }
    catch [Management.Automation.ParameterBindingException] { $rejected = $true }
    if (-not $rejected) { throw "Invalid target accepted: $target" }
}
$global:LASTEXITCODE = 0
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "launcher-test.ps1"
            path.write_text(script, encoding="ascii")
            result = subprocess.run(
                [powershell, "-NoProfile", "-NonInteractive", "-File", str(path),
                 "-Launcher", str(DM / "windows" / "run.ps1"), "-Root", directory],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_loader_target_rejection(self):
        if not ARGS.loader:
            self.skipTest("pass --loader to test the native loader")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.fixups.tsv"
            header = "# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0x4000\n# KEY_PAGE_SIZE\t0x4000\n"
            header += "".join(f"# KEY_{key}_IDX\t{i}\n" for key, i in encoders()["KEY_INDEX"].items())
            for target in (3, 0, 1, 2, 4, None):
                path.write_text(header + (f"# TARGET\t{target:#x}\n" if target is not None else ""), encoding="ascii")
                result = subprocess.run([ARGS.loader, "--layout", directory], capture_output=True, timeout=20)
                if target == 3:
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(b"TARGET", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--emit", type=Path)
    parser.add_argument("--loader")
    ARGS, rest = parser.parse_known_args()
    if ARGS.emit:
        emit_blocks(ARGS.emit)
    else:
        unittest.main(argv=[sys.argv[0], *rest])
