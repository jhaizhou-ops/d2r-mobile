#!/usr/bin/env python3
# 用壳里 NativeAOT 预编的翻译工具跑着色器出库链（给 tools/d2rm_pack.py 用；做包的机器上不需要 .NET）。
# 用法：aot_run.py <脚本.py> [参数…]   例：aot_run.py dmsh_build.py <包目录> <缓存目录>；可套在 nometal_run.py 外面
# 做法同 nometal_run.py：出库链的文件内容参与翻译链版本，一个字都不改，只在本进程里接过一处——dmsh_chain.py 记工具指纹时读翻译工具目录里的
# dmsh_xlate.dll / Ryujinx.Graphics.Shader.dll / Ryujinx.Common.dll，NativeAOT 单文件没有这些 dll：这三个路径在不存在时改读同目录的可执行文件。
# 指纹于是和 SDK 现编那套不同，d2rm_pack.py 照旧按本机工具重新登记；翻出来的着色器对不对仍由源码摘要（壳的 SHADER_SRC）把关。
# Runs the shader build chain with the shell's NativeAOT prebuilt translator (for tools/d2rm_pack.py; the packing machine needs no .NET).
# Usage: aot_run.py <script.py> [args...]   Example: aot_run.py dmsh_build.py <package dir> <cache dir>; it can wrap nometal_run.py
# Same approach as nometal_run.py: the build chain's file contents feed into the translation chain version, so not a character of them changes; only one thing is taken over in this process -- when dmsh_chain.py
# fingerprints the tools it reads dmsh_xlate.dll / Ryujinx.Graphics.Shader.dll / Ryujinx.Common.dll in the translator directory, which a NativeAOT single file does not have: when those paths do not exist, the executable next to them is read instead.
# The fingerprints then differ from the SDK-built set and d2rm_pack.py re-registers by the local tools as before; whether the translated shaders are right is still guarded by the source digest (the shell's SHADER_SRC).
import builtins, os, runpy, sys

DLLS = ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll')
_open = builtins.open


def _aot_open(file, *a, **k):
    if isinstance(file, str) and os.path.basename(file) in DLLS and not os.path.exists(file):
        d = os.path.dirname(file)
        exe = next((os.path.join(d, e) for e in ('dmsh_xlate.exe', 'dmsh_xlate') if os.path.isfile(os.path.join(d, e))), None)
        if exe: return _open(exe, *a, **k)
    return _open(file, *a, **k)


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit('用法：aot_run.py <脚本.py> [参数…] | usage: aot_run.py <script.py> [args...]')
    builtins.open = _aot_open
    script = os.path.abspath(sys.argv[1]); sys.argv = sys.argv[1:]; sys.path.insert(0, os.path.dirname(script))
    runpy.run_path(script, run_name='__main__')
