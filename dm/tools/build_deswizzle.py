#!/usr/bin/env python3
"""用 Windows LLVM 构建资源生成器所需的本机纹理 DLL，不需要游戏文件或 Windows SDK。
Build the generators' native texture DLL with Windows LLVM, without game files or a Windows SDK.
"""
import argparse
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

DM = Path(__file__).resolve().parents[1]
EXPORTS = ('nvn_tex_layout', 'nvn_deswizzle', 'nvn_swizzle')

# 原算法不变；freestanding 编译只需补这两个 C 库函数，避免依赖本机 CRT。
# Keep the algorithms unchanged; these two C functions avoid a host CRT dependency in a freestanding build.
STRING_H = '''#pragma once
#include <stddef.h>
void *memcpy(void *restrict dest, const void *restrict src, size_t count);
void *memset(void *dest, int value, size_t count);
'''
MEMORY_C = '''#include <stddef.h>
void *memcpy(void *restrict dest, const void *restrict src, size_t count) {
    volatile unsigned char *d = dest;
    const volatile unsigned char *s = src;
    for (size_t i = 0; i < count; ++i) d[i] = s[i];
    return dest;
}
void *memset(void *dest, int value, size_t count) {
    volatile unsigned char *d = dest;
    for (size_t i = 0; i < count; ++i) d[i] = (unsigned char)value;
    return dest;
}
'''


def library_error(path):
    """在子进程中验证 DLL，退出后释放文件锁。 | Validate in a child process so its DLL file lock is released."""
    try:
        library = Path(path).expanduser().resolve()
        if not library.is_file():
            return 'file not found: %s' % library
    except OSError as error:
        return 'cannot access native texture library: %s' % error
    probe = ('import ctypes, sys; library = ctypes.CDLL(sys.argv[1]); '
             'missing = [name for name in sys.argv[2:] if not hasattr(library, name)]; '
             'sys.exit("missing exports: " + ", ".join(missing) if missing else 0)')
    try:
        result = subprocess.run([sys.executable, '-c', probe, str(library), *EXPORTS],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
                                env=dict(os.environ, PYTHONUTF8='1'))
    except (OSError, subprocess.TimeoutExpired) as error:
        return 'cannot check native texture library: %s' % error
    if result.returncode:
        lines = result.stderr.decode('utf-8', 'replace').strip().splitlines()
        return lines[-1] if lines else 'native texture library probe failed (%d)' % result.returncode
    return None


def texture_dependency_error(environ=None):
    """打包前检查指定库，或确认自动编译所需的 clang 存在。 | Check an explicit library or clang before packaging."""
    environ = os.environ if environ is None else environ
    library = environ.get('DM_DESWIZZLE_LIB')
    if library:
        error = library_error(library)
        return 'DM_DESWIZZLE_LIB: %s' % error if error else None
    if shutil.which('clang'):
        return None
    return ('C compiler (clang) is missing. Install a working host C toolchain, or set '
            'DM_DESWIZZLE_LIB to a native texture library. Windows x64: '
            'python dm/tools/build_deswizzle.py --llvm-bin <LLVM bin> --output <work>/deswizzle.dll '
            '(see docs/ANDROID.md).')


def build(llvm_bin, output):
    """原子发布 DLL；编译失败不替换已有库。 | Publish atomically; a failed build preserves an existing library."""
    if (os.name != 'nt' or struct.calcsize('P') != 8
            or platform.machine().lower() not in ('amd64', 'x86_64')):
        raise ValueError('run this helper with x64 Python on Windows')
    llvm_bin = Path(llvm_bin).expanduser().resolve()
    compiler = llvm_bin / 'clang.exe'
    if not compiler.is_file():
        raise ValueError('clang.exe not found in --llvm-bin: %s' % llvm_bin)
    linker = None
    for name in ('ld.lld.exe', 'lld-link.exe', 'lld.exe'):
        path = llvm_bin / name
        if path.is_file():
            linker = [str(path)] + ([] if name == 'lld-link.exe' else ['-flavor', 'link'])
            break
    if linker is None:
        raise ValueError('--llvm-bin needs ld.lld.exe, lld-link.exe or lld.exe')
    output = Path(output).expanduser().resolve()
    if output.suffix.lower() != '.dll' or output.is_relative_to(DM):
        raise ValueError('--output must be a .dll outside dm/ (for example in your DM_ROOT)')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dm-deswizzle-', dir=output.parent) as temporary:
        work = Path(temporary)
        (work / 'string.h').write_text(STRING_H, encoding='utf-8')
        (work / 'memory.c').write_text(MEMORY_C, encoding='utf-8')
        # 编译子进程也使用这个真实临时目录，避免 Windows 沙箱的临时路径重定向。
        # Give compiler children this real temporary directory too, avoiding Windows sandbox temp redirection.
        env = dict(os.environ, TEMP=str(work), TMP=str(work))
        sources = (DM / 'gfx/tex/nvn_deswizzle.c', DM / 'gfx/nvn/nvn_tex.c', work / 'memory.c')
        objects = []
        for source in sources:
            obj = work / (source.stem + '.obj')
            subprocess.run([str(compiler), '--target=x86_64-pc-windows-msvc', '-O2', '-c',
                            '-ffreestanding', '-fno-stack-protector', '-I' + str(work),
                            '-I' + str(DM / 'gfx/nvn'), '-I' + str(DM / 'gfx/tex'),
                            '-o', str(obj), str(source)], env=env, check=True)
            objects.append(str(obj))
        library = work / 'deswizzle.dll'
        subprocess.run(linker + ['/dll', '/noentry', '/nodefaultlib', '/out:' + str(library)]
                       + ['/export:' + name for name in EXPORTS] + objects, env=env, check=True)
        error = library_error(library)
        if error:
            raise ValueError(error)
        os.replace(library, output)
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm-bin', type=Path, required=True, help='directory containing Windows clang.exe and LLD')
    parser.add_argument('--output', type=Path, required=True, help='native x64 DLL outside dm/')
    args = parser.parse_args(argv)
    try:
        output = build(args.llvm_bin, args.output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print('build_deswizzle: %s' % error, file=sys.stderr)
        return 1
    print('Built and checked: %s' % output)
    print("$env:DM_DESWIZZLE_LIB = '%s'" % str(output).replace("'", "''"))
    return 0


if __name__ == '__main__':
    sys.exit(main())
