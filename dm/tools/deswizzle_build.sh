#!/bin/bash
# 预编贴图解排布库（界面素材脚本 dm/ios/tools/romfs_art.py、gen_widestash_art.py 用 ctypes 调；同一份源码 dm/gfx/tex/nvn_deswizzle.c + dm/gfx/nvn/nvn_tex.c）。
#   用法：deswizzle_build.sh <输出目录>  → <输出目录>/macos-arm64/deswizzle.dylib、<输出目录>/win-x64/deswizzle.dll、<输出目录>/source.sha256
#   出壳时随壳走（壳 zip 顶层 Tools/deswizzle/），d2rm_pack.py 按本机平台取一份设 DM_DESWIZZLE_LIB：做包的机器上不用 C 编译器（S2Arm Windows 包里就没有）。
#   Windows 那份在 Mac 上交叉编：不连 C 运行库（只用到 memcpy / memset，自带一份），所以不需要 Windows SDK，玩家机器上也不缺运行库。
# Prebuilt texture deswizzle library (called through ctypes by the interface-art scripts dm/ios/tools/romfs_art.py and gen_widestash_art.py; same sources dm/gfx/tex/nvn_deswizzle.c + dm/gfx/nvn/nvn_tex.c).
#   Usage: deswizzle_build.sh <out dir> → <out dir>/macos-arm64/deswizzle.dylib, <out dir>/win-x64/deswizzle.dll, <out dir>/source.sha256
#   Ships with the shell (top-level Tools/deswizzle/ of the shell zip); d2rm_pack.py takes the one for its platform and sets DM_DESWIZZLE_LIB, so the packing machine needs no C compiler (the S2Arm Windows package has none).
#   The Windows one is cross-built on the Mac without the C runtime (only memcpy / memset are used, and a copy of those is included), so no Windows SDK is needed and nothing is missing on the player's machine.
set -euo pipefail
O=${1:?输出目录 | output dir}; H=$(cd "$(dirname "$0")/.." && pwd)
SRC=("$H/gfx/tex/nvn_deswizzle.c" "$H/gfx/nvn/nvn_tex.c"); INC=(-I"$H/gfx/nvn" -I"$H/gfx/tex")
LLVM=${DM_LLVM:-$(ls -d /opt/homebrew/opt/llvm@*/bin 2> /dev/null | while read -r d; do [ -x "$d/lld-link" ] && echo "$d"; done | head -1 || true)}
[ -n "$LLVM" ] && [ -x "$LLVM/lld-link" ] || { echo "[x] 找不到带 lld-link 的 LLVM（brew install llvm，或 DM_LLVM=<bin 目录>）| no LLVM with lld-link found" >&2; exit 1; }
CC=$LLVM/clang; [ -x "$CC" ] || CC=clang   # 交叉编 Windows 那份：苹果自带的 clang 也认 x86_64-pc-windows-msvc，只缺链接器 | for the Windows one: Apple's clang also accepts x86_64-pc-windows-msvc, only the linker is missing
rm -rf "$O" && mkdir -p "$O/macos-arm64" "$O/win-x64"
clang -O2 -shared -fPIC -arch arm64 "-ffile-prefix-map=$H=/_/" -Wl,-install_name,@rpath/deswizzle.dylib "${INC[@]}" -o "$O/macos-arm64/deswizzle.dylib" "${SRC[@]}"   # 不带本机路径：安装名与源码路径都换掉（出壳核对本机路径）| no local paths: install name and source paths replaced
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; mkdir "$W/inc"
printf '#pragma once\n#include <stddef.h>\nvoid *memcpy(void *d, const void *s, size_t n);\nvoid *memset(void *d, int c, size_t n);\n' > "$W/inc/string.h"
printf '#include <stddef.h>\nvoid *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }\nvoid *memset(void *d, int c, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }\n' > "$W/crt.c"
for f in "${SRC[@]}" "$W/crt.c"; do "$CC" --target=x86_64-pc-windows-msvc -O2 "-ffile-prefix-map=$H=/_/" "-ffile-prefix-map=$W=/_/" -ffreestanding -fno-builtin -isystem "$W/inc" "${INC[@]}" -c "$f" -o "$W/$(basename "$f" .c).obj"; done
"$LLVM/lld-link" /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /EXPORT:nvn_tex_layout /EXPORT:nvn_deswizzle /EXPORT:nvn_swizzle /OUT:"$O/win-x64/deswizzle.dll" "$W"/*.obj > /dev/null
rm -f "$O/win-x64/deswizzle.lib" "$O/win-x64/deswizzle.exp"
cat "${SRC[@]}" "$H/gfx/tex/nvn_deswizzle.h" "$H/gfx/nvn/nvn_tex.h" | shasum -a 256 | cut -d' ' -f1 > "$O/source.sha256"
echo "贴图解排布库：$O（macos-arm64 $(stat -f %z "$O/macos-arm64/deswizzle.dylib") 字节，win-x64 $(stat -f %z "$O/win-x64/deswizzle.dll") 字节，源码 $(cut -c1-12 "$O/source.sha256")）"
