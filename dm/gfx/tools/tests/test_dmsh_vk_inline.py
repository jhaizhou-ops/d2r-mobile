#!/usr/bin/env python3
# 用手写的最小 SPIR-V 测试内联；不含游戏着色器。只提取函数，避免执行打包脚本的命令行入口。
# Test inlining with hand-authored minimal SPIR-V, without game shaders. Extract functions only to avoid the packer's CLI entry point.
import ast
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / 'dmsh_vk.py'
NAMES = {'helper_has_subgroup', 'inline_subgroup_helpers'}
tree = ast.parse(SOURCE.read_text(encoding='utf-8'))
ns = {'struct': struct, 'subprocess': subprocess, 'OPT': os.environ.get('SPIRV_OPT', 'spirv-opt')}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in NAMES], type_ignores=[]), str(SOURCE), 'exec'), ns)
has_subgroup = ns['helper_has_subgroup']
inline_helpers = ns['inline_subgroup_helpers']
VAL = os.environ.get('SPIRV_VAL', 'spirv-val')


def module(stage, in_helper=True):
    def op(code, *args): return [(len(args) + 1) << 16 | code, *args]
    w = [0x07230203, 0x00010300, 0, 23, 0]
    w += op(17, 1) + op(17, 61) + op(17, 65)
    w += op(14, 0, 1) + op(15, stage, 10, 0x6e69616d, 0)
    w += op(16, 10, 17, 1, 1, 1) if stage == 5 else op(16, 10, 7)
    w += op(19, 1) + op(21, 2, 32, 0) + op(33, 3, 1) + op(33, 4, 2)
    w += op(43, 2, 5, 3) + op(43, 2, 6, 0)
    w += op(54, 1, 10, 0, 3) + op(248, 11)
    w += op(57, 2, 12, 20) if in_helper else op(345, 2, 12, 5, 6, 6)
    w += op(253) + op(56)
    if in_helper:
        w += op(54, 2, 20, 0, 4) + op(248, 21) + op(345, 2, 22, 5, 6, 6) + op(254, 22) + op(56)
    return struct.pack('<%dI' % len(w), *w)


class InlineTests(unittest.TestCase):
    def validate(self, p):
        r = subprocess.run([VAL, '--target-env', 'vulkan1.1', str(p)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_compute_and_fragment_helpers_are_inlined_and_valid(self):
        for stage in (5, 4):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as d:
                src, dst = str(Path(d) / 'in.spv'), str(Path(d) / 'out.spv')
                spv = module(stage); Path(src).write_bytes(spv); self.validate(src)
                self.assertTrue(has_subgroup(spv))
                result = inline_helpers(spv, src, dst)
                self.assertFalse(has_subgroup(result)); self.validate(dst)
                self.assertNotEqual(result, spv)

    def test_entry_only_subgroup_is_byte_identical_without_optimizer(self):
        for stage in (5, 4):
            with self.subTest(stage=stage), patch.object(subprocess, 'run', side_effect=AssertionError('unexpected optimizer')):
                spv = module(stage, False)
                self.assertFalse(has_subgroup(spv))
                self.assertEqual(inline_helpers(spv, 'unused', 'unused'), spv)

    def test_optimizer_failure_is_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as d, patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'intentional failure')):
            with self.assertRaisesRegex(ValueError, 'intentional failure'): inline_helpers(module(4), str(Path(d) / 'in'), str(Path(d) / 'out'))

    def test_remaining_helper_is_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            dst = Path(d) / 'out'; dst.write_bytes(module(4))
            with patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')):
                with self.assertRaises(ValueError): inline_helpers(module(4), str(Path(d) / 'in'), str(dst))


if __name__ == '__main__': unittest.main()
