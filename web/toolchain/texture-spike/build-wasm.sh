#!/usr/bin/env bash
# 构建纹理转换 WASM 桥接；依据 web/toolchain/texture-spike 的模块与调用约定。
# Build the texture-conversion WASM bridge using the module and calling contracts in web/toolchain/texture-spike.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${TEXTURE_OUTPUT_DIR:-"$HERE/dist"}
emcc --version | head -1 | grep -F '4.0.20' >/dev/null || { echo 'Expected official Emscripten 4.0.20' >&2; exit 1; }
mkdir -p "$OUT/licenses"
emcc -O2 -std=c11 -I"$ROOT/dm/gfx/nvn" -I"$ROOT/dm/gfx/tex" \
  "$HERE/boundary.c" "$ROOT/dm/gfx/nvn/nvn_tex.c" \
  "$ROOT/dm/gfx/tex/nvn_deswizzle.c" "$ROOT/dm/gfx/tex/nvn_bcdec.c" \
  --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=worker,node \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=16777216 -sMAXIMUM_MEMORY=268435456 \
  -sSTACK_SIZE=1048576 -sFILESYSTEM=0 -sABORTING_MALLOC=0 \
  '-sEXPORTED_RUNTIME_METHODS=["HEAPU8","HEAPU32"]' \
  '-sEXPORTED_FUNCTIONS=["_malloc","_free","_texture_info","_texture_decode","_texture_decode_pillow_bc3","_texture_pack_bc3"]' \
  -o "$OUT/texture.mjs"
cp "$ROOT/LICENSE" "$OUT/licenses/repository-LICENSE.txt"
cp "$ROOT/dm/third_party/bcdec/LICENSE" "$OUT/licenses/bcdec-LICENSE.txt"
cp "$ROOT/dm/third_party/bcdec/bcdec.h" "$OUT/licenses/bcdec-source-notices.txt"
cp "$ROOT/dm/gfx/nvn/nvn_tex.c" "$OUT/licenses/nvn-layout-attribution.txt"
# 此桥接保留 deko3d 的 zlib 声明及原始来源署名。
# deko3d's zlib notice is retained with this bridge, alongside original source attribution.
cp "$HERE/deko3d-LICENSE.txt" "$OUT/licenses/deko3d-LICENSE.txt"
python3 "$HERE/package-encoder.py"
(cd "$OUT" && sha256sum texture.mjs texture.wasm > SHA256SUMS)
