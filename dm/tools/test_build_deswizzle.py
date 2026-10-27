#!/usr/bin/env python3
"""不读游戏文件的纹理依赖预检与 Windows DLL 构建测试。
Texture dependency and Windows DLL build checks that do not read game files.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_deswizzle as helper


class DependencyChecks(unittest.TestCase):
    def test_missing_compiler_has_actionable_hint(self):
        with patch.object(helper.shutil, 'which', return_value=None):
            error = helper.texture_dependency_error({})
        self.assertIn('clang', error)
        self.assertIn('--llvm-bin', error)
        self.assertIn('DM_DESWIZZLE_LIB', error)

    def test_missing_explicit_library_does_not_fall_back_to_clang(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / 'missing library.dll'
            with patch.object(helper.shutil, 'which', return_value='clang'):
                error = helper.texture_dependency_error({'DM_DESWIZZLE_LIB': str(missing)})
        self.assertIn('DM_DESWIZZLE_LIB', error)
        self.assertIn('file not found', error)

    def test_invalid_native_library_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            library = Path(temporary) / 'invalid library.dll'
            library.write_bytes(b'not a native library')
            self.assertIsNotNone(helper.texture_dependency_error({'DM_DESWIZZLE_LIB': str(library)}))
            # 预检子进程退出后应能重命名，不能锁住用户指定的文件。
            # The probe child must release the file so the user can rename it.
            library.rename(Path(temporary) / 'renamed.dll')


# 用项目 C 结构的布局检查编译器 ABI，随后验证不整块尺寸的压缩纹理往返。
# Check the compiler ABI against the project's C structs, then round-trip non-block-aligned compressed textures.
ROUNDTRIP = r'''
import ctypes as C
import sys
class Desc(C.Structure):
    _fields_ = [(n, C.c_int) for n in ('target', 'format', 'flags', 'samples')] + [(n, C.c_uint32) for n in ('width', 'height', 'depth', 'levels', 'stride')] + [('pkg_block_h', C.c_int)]
class Layout(C.Structure):
    _fields_ = [('size', C.c_uint64), ('layer_size', C.c_uint64)] + [(n, C.c_uint32) for n in ('align', 'layers', 'bpb', 'tileH', 'tileD')]
lib = C.CDLL(sys.argv[1])
lib.nvn_tex_layout.argtypes = [C.POINTER(Desc), C.POINTER(Layout)]
lib.nvn_swizzle.argtypes = [C.POINTER(Desc), C.c_uint, C.c_uint, C.c_void_p, C.c_size_t, C.c_void_p]
lib.nvn_deswizzle.argtypes = [C.POINTER(Desc), C.c_uint, C.c_uint, C.c_void_p, C.c_void_p, C.c_size_t]
for w, h in ((16, 16), (17, 33), (256, 192), (1920, 1080)):
    desc, layout = Desc(1, 0x44, 0, 0, w, h, 1, 1, 0, 0), Layout()
    lib.nvn_tex_layout(C.byref(desc), C.byref(layout))
    pitch = (w + 3) // 4 * 16
    raw = bytes((i * 37 + 19) % 256 for i in range(pitch * ((h + 3) // 4)))
    if layout.bpb != 16 or layout.layers != 1 or layout.size < len(raw):
        raise SystemExit('texture layout ABI mismatch')
    src, tiled, dst = C.create_string_buffer(raw, len(raw)), C.create_string_buffer(layout.size), C.create_string_buffer(len(raw))
    if lib.nvn_swizzle(C.byref(desc), 0, 0, src, pitch, tiled) != 0:
        raise SystemExit('swizzle failed')
    if lib.nvn_deswizzle(C.byref(desc), 0, 0, tiled, dst, pitch) != 0 or dst.raw != raw:
        raise SystemExit('deswizzle roundtrip mismatch')
print('4 texture roundtrips OK')
'''


@unittest.skipUnless(os.name == 'nt' and os.environ.get('DM_TEST_LLVM_BIN'),
                     'set DM_TEST_LLVM_BIN to a Windows LLVM/NDK bin directory')
class WindowsBuild(unittest.TestCase):
    def test_loadable_library_without_texture_exports_is_rejected(self):
        python_dll = Path(sys.base_prefix) / ('python%d%d.dll' % sys.version_info[:2])
        error = helper.library_error(python_dll)
        self.assertIn('missing exports', error)
        for name in helper.EXPORTS:
            self.assertIn(name, error)

    def test_build_and_rebuild_with_spaces_and_native_roundtrip(self):
        with tempfile.TemporaryDirectory(prefix='dm texture test ') as temporary:
            output = Path(temporary) / 'output with spaces' / 'deswizzle.dll'
            helper.build(os.environ['DM_TEST_LLVM_BIN'], output)
            self.assertIsNone(helper.texture_dependency_error({'DM_DESWIZZLE_LIB': str(output)}))
            result = subprocess.run([sys.executable, '-c', ROUNDTRIP, str(output)],
                                    capture_output=True, text=True, check=True)
            self.assertIn('4 texture roundtrips OK', result.stdout)
            # 同一路径可再次构建：预检不能让 DLL 留在父 Python 进程里。
            # Rebuilding at the same path also checks that the probe did not load the DLL in the parent.
            helper.build(os.environ['DM_TEST_LLVM_BIN'], output)

    def test_missing_linker_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'clang.exe').write_bytes(b'not executed')
            output = root / 'keep.dll'
            output.write_bytes(b'existing output')
            with self.assertRaisesRegex(ValueError, 'lld'):
                helper.build(root, output)
            self.assertEqual(output.read_bytes(), b'existing output')


if __name__ == '__main__':
    unittest.main()
